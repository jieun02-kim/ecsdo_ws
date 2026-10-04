/*
 * libecsdo v2: handle, dictionary readiness, dictionary read, upload.
 * All IgH access goes through ecsdo_ioctl.c.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo_ioctl.h"

/* Readiness polling: interval and consecutive equal readings. */
#define READY_POLL_MS 100u
#define READY_STABLE_READS 3u

typedef enum {
    READY_UNKNOWN = 0,        /* ecsdo_wait_ready() not called */
    READY_YES,
    READY_NO
} ready_t;

struct ecsdo_ctx {
    int fd;
    int writable;
    int last_errno;
    uint16_t ready_count;     /* size of ready[] */
    ready_t *ready;           /* by slave position, from wait_ready */
};

static int io_fail(ecsdo_ctx_t *ctx, int neg_errno)
{
    ctx->last_errno = -neg_errno;
    return ECSDO_ERR_IOCTL;
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned int ms)
{
    struct timespec ts = {ms / 1000u, (long) (ms % 1000u) * 1000000L};

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* ------------------------------------------------------------------------
 * Handle
 * ---------------------------------------------------------------------- */

int ecsdo_open(ecsdo_ctx_t **out, unsigned int master_index)
{
    ecsdo_ctx_t *ctx;
    uint32_t magic, master_count;
    int fd, writable = 1, ret;

    if (!out)
        return ECSDO_ERR_INVALID_ARG;
    *out = NULL;

    ret = ecsdo_io_open(master_index, 1, &fd);
    if (ret == -EACCES || ret == -EPERM || ret == -EROFS) {
        writable = 0; /* dictionary still readable, upload not */
        ret = ecsdo_io_open(master_index, 0, &fd);
    }
    if (ret) {
        errno = -ret;
        return ECSDO_ERR_OPEN;
    }

    ret = ecsdo_io_module(fd, &magic, &master_count);
    if (ret || magic != ecsdo_io_expected_magic()) {
        ecsdo_io_close(fd);
        if (ret) {
            errno = -ret;
            return ECSDO_ERR_IOCTL;
        }
        return ECSDO_ERR_VERSION;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ecsdo_io_close(fd);
        return ECSDO_ERR_NO_MEMORY;
    }
    ctx->fd = fd;
    ctx->writable = writable;
    *out = ctx;
    return ECSDO_OK;
}

void ecsdo_close(ecsdo_ctx_t *ctx)
{
    if (!ctx)
        return;
    ecsdo_io_close(ctx->fd);
    free(ctx->ready);
    free(ctx);
}

int ecsdo_last_errno(const ecsdo_ctx_t *ctx)
{
    return ctx ? ctx->last_errno : 0;
}

int ecsdo_slave_count(ecsdo_ctx_t *ctx, uint16_t *count)
{
    ecsdo_io_master_t m;
    int ret;

    if (!ctx || !count)
        return ECSDO_ERR_INVALID_ARG;
    ret = ecsdo_io_master(ctx->fd, &m);
    if (ret)
        return io_fail(ctx, ret);
    *count = (uint16_t) m.slave_count;
    return ECSDO_OK;
}

/* ------------------------------------------------------------------------
 * Dictionary readiness
 * ---------------------------------------------------------------------- */

/** What the master currently holds of a slave's dictionary. */
typedef struct {
    uint16_t sdo_count;
    uint16_t last_index;
    uint8_t last_max_subindex;
    uint16_t last_entries;
    char last_name[ECSDO_STRING_SIZE];
} dict_sig_t;

static int al_allows_dict(uint8_t al_state)
{
    uint8_t s = al_state & ECSDO_AL_STATE_MASK;

    return s == ECSDO_AL_PREOP || s == ECSDO_AL_SAFEOP || s == ECSDO_AL_OP;
}

/** Same rule as the master: without a general SII category it still uses
 *  the SDO information service. */
static int has_dict(const ecsdo_io_slave_t *s)
{
    return (s->mailbox_protocols & ECSDO_MBOX_COE)
        && (!s->has_general_category || s->enable_sdo_info);
}

/** Snapshot the last object of the dictionary. Returns 0 or -errno. */
static int dict_signature(int fd, uint16_t position, uint16_t sdo_count,
        dict_sig_t *sig)
{
    ecsdo_io_sdo_t sdo;
    ecsdo_io_sdo_entry_t entry;
    unsigned int sub;
    int ret;

    memset(sig, 0, sizeof(*sig));
    sig->sdo_count = sdo_count;
    if (!sdo_count)
        return 0;

    ret = ecsdo_io_sdo(fd, position, sdo_count - 1, &sdo);
    if (ret)
        return ret;
    sig->last_index = sdo.index;
    sig->last_max_subindex = sdo.max_subindex;
    memcpy(sig->last_name, sdo.name, sizeof(sig->last_name));

    for (sub = 0; sub <= sdo.max_subindex; sub++) {
        ret = ecsdo_io_sdo_entry(fd, position, sdo_count - 1,
                (uint8_t) sub, &entry);
        if (ret == -EINVAL)
            continue; /* not (yet) fetched */
        if (ret)
            return ret;
        sig->last_entries++;
    }
    return 0;
}

/** The master fills objects in list order: entries on the last object
 *  mean it got to the end. */
static int dict_sig_complete(const dict_sig_t *sig)
{
    return sig->sdo_count > 0 && sig->last_entries > 0;
}

static int dict_sig_equal(const dict_sig_t *a, const dict_sig_t *b)
{
    return a->sdo_count == b->sdo_count && a->last_index == b->last_index
        && a->last_max_subindex == b->last_max_subindex
        && a->last_entries == b->last_entries
        && !strcmp(a->last_name, b->last_name);
}

typedef struct {
    ready_t ready;
    unsigned int stable;
    dict_sig_t sig;
} wait_slave_t;

/** Poll one slave once. Returns 0 or -errno. */
static int poll_slave(ecsdo_ctx_t *ctx, uint16_t position, wait_slave_t *w)
{
    ecsdo_io_slave_t s;
    dict_sig_t sig;
    int ret, complete;

    ret = ecsdo_io_slave(ctx->fd, position, &s);
    if (ret)
        return ret;
    if (!(s.al_state & ECSDO_AL_STATE_MASK)) {
        w->stable = 0; /* not scanned yet */
        return 0;
    }
    if (!has_dict(&s)) {
        w->ready = READY_YES; /* no dictionary to wait for */
        return 0;
    }
    if (!al_allows_dict(s.al_state) || !s.sdo_count) {
        w->stable = 0;
        return 0;
    }

    ret = dict_signature(ctx->fd, position, s.sdo_count, &sig);
    if (ret == -EINVAL) {
        w->stable = 0; /* dictionary changed under us */
        return 0;
    }
    if (ret)
        return ret;

    complete = dict_sig_complete(&sig);
    if (complete && w->stable && dict_sig_equal(&sig, &w->sig))
        w->stable++;
    else
        w->stable = complete ? 1 : 0;
    w->sig = sig;
    if (w->stable >= READY_STABLE_READS)
        w->ready = READY_YES;
    return 0;
}

int ecsdo_wait_ready(ecsdo_ctx_t *ctx, unsigned int timeout_ms)
{
    ecsdo_io_master_t m;
    wait_slave_t *w = NULL;
    uint32_t count = 0, i;
    uint64_t deadline;
    int ret, all_ready;

    if (!ctx)
        return ECSDO_ERR_INVALID_ARG;
    deadline = now_ms() + timeout_ms;

    for (;;) {
        ret = ecsdo_io_master(ctx->fd, &m);
        if (ret) {
            free(w);
            return io_fail(ctx, ret);
        }
        if (m.scan_busy || m.slave_count != count || !w) {
            /* (Re)start: a new scan invalidates everything. */
            free(w);
            w = NULL;
            count = m.slave_count;
            if (count && !(w = calloc(count, sizeof(*w))))
                return ECSDO_ERR_NO_MEMORY;
        }

        all_ready = !m.scan_busy;
        for (i = 0; !m.scan_busy && i < count; i++) {
            if (w[i].ready == READY_YES)
                continue;
            ret = poll_slave(ctx, (uint16_t) i, &w[i]);
            if (ret == -EINVAL) {
                w[i].stable = 0; /* slave vanished: topology change */
            } else if (ret) {
                free(w);
                return io_fail(ctx, ret);
            }
            if (w[i].ready != READY_YES)
                all_ready = 0;
        }

        if (all_ready || now_ms() >= deadline)
            break;
        sleep_ms(READY_POLL_MS);
    }

    free(ctx->ready);
    ctx->ready = NULL;
    ctx->ready_count = 0;
    if (count) {
        ctx->ready = calloc(count, sizeof(*ctx->ready));
        if (!ctx->ready) {
            free(w);
            return ECSDO_ERR_NO_MEMORY;
        }
        ctx->ready_count = (uint16_t) count;
        for (i = 0; i < count; i++)
            ctx->ready[i] = (!m.scan_busy && w[i].ready == READY_YES)
                ? READY_YES : READY_NO;
    }
    free(w);
    return all_ready ? ECSDO_OK : ECSDO_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------------
 * Dictionary
 * ---------------------------------------------------------------------- */

void ecsdo_free_slave(ecsdo_slave_t *slave)
{
    size_t i;

    if (!slave)
        return;
    for (i = 0; i < slave->object_count; i++)
        free(slave->objects[i].entries);
    free(slave->objects);
    free(slave);
}

/** Is the dictionary known (or, without wait_ready, believed) complete? */
static int dict_ready(ecsdo_ctx_t *ctx, const ecsdo_slave_t *slave,
        uint16_t sdo_count)
{
    dict_sig_t sig;

    if (slave->position < ctx->ready_count
            && ctx->ready[slave->position] != READY_UNKNOWN)
        return ctx->ready[slave->position] == READY_YES;
    /* No wait_ready() result: one check without the stability polls. */
    if (!al_allows_dict(slave->al_state)
            || dict_signature(ctx->fd, slave->position, sdo_count, &sig))
        return 0;
    return dict_sig_complete(&sig);
}

/** Object at list position pos; partial results stay in obj. 0 or -errno. */
static int read_object(ecsdo_ctx_t *ctx, uint16_t position, uint16_t pos,
        ecsdo_object_t *obj)
{
    ecsdo_io_sdo_t sdo;
    ecsdo_io_sdo_entry_t io;
    ecsdo_entry_t *e;
    unsigned int sub;
    int ret;

    ret = ecsdo_io_sdo(ctx->fd, position, pos, &sdo);
    if (ret)
        return ret;
    obj->index = sdo.index;
    obj->max_subindex = sdo.max_subindex;
    memcpy(obj->name, sdo.name, sizeof(obj->name));

    obj->entries = calloc((size_t) sdo.max_subindex + 1,
            sizeof(*obj->entries));
    if (!obj->entries)
        return -ENOMEM;

    for (sub = 0; sub <= sdo.max_subindex; sub++) {
        ret = ecsdo_io_sdo_entry(ctx->fd, position, pos, (uint8_t) sub,
                &io);
        if (ret == -EINVAL)
            continue; /* subindex not in the dictionary */
        if (ret)
            return ret;
        e = &obj->entries[obj->entry_count++];
        e->subindex = (uint8_t) sub;
        e->data_type = io.data_type;
        e->bit_length = io.bit_length;
        memcpy(e->read_access, io.read_access, sizeof(e->read_access));
        memcpy(e->write_access, io.write_access, sizeof(e->write_access));
        memcpy(e->description, io.description, sizeof(e->description));
    }
    return 0;
}

int ecsdo_read_dict(ecsdo_ctx_t *ctx, uint16_t position,
        ecsdo_slave_t **out)
{
    ecsdo_io_master_t m;
    ecsdo_io_slave_t s;
    ecsdo_slave_t *slave;
    uint16_t i;
    int ret;

    if (!ctx || !out)
        return ECSDO_ERR_INVALID_ARG;
    *out = NULL;

    ret = ecsdo_io_master(ctx->fd, &m);
    if (ret)
        return io_fail(ctx, ret);
    ret = ecsdo_io_slave(ctx->fd, position, &s);
    if (ret == -EINVAL)
        return ECSDO_ERR_NO_SLAVE;
    if (ret)
        return io_fail(ctx, ret);

    slave = calloc(1, sizeof(*slave));
    if (!slave)
        return ECSDO_ERR_NO_MEMORY;
    slave->position = position;
    slave->alias = s.alias;
    slave->vendor_id = s.vendor_id;
    slave->product_code = s.product_code;
    slave->revision = s.revision;
    slave->serial = s.serial;
    memcpy(slave->name, s.name, sizeof(slave->name));
    slave->al_state = s.al_state;
    slave->coe = (s.mailbox_protocols & ECSDO_MBOX_COE) ? 1 : 0;
    slave->sdo_info = has_dict(&s) ? 1 : 0;

    if (m.scan_busy || !(s.al_state & ECSDO_AL_STATE_MASK)) {
        /* SII not read yet: "no CoE" would be a guess. */
        slave->status = ECSDO_SLAVE_NOT_READY;
        goto done;
    }
    if (!slave->coe) {
        slave->status = ECSDO_SLAVE_NO_COE;
        goto done;
    }
    if (!slave->sdo_info) {
        slave->status = ECSDO_SLAVE_NO_SDO_INFO;
        goto done;
    }
    if (!s.sdo_count) {
        slave->status = ECSDO_SLAVE_NOT_READY;
        goto done;
    }

    slave->status = dict_ready(ctx, slave, s.sdo_count)
        ? ECSDO_SLAVE_OK : ECSDO_SLAVE_NOT_READY;

    slave->objects = calloc(s.sdo_count, sizeof(*slave->objects));
    if (!slave->objects) {
        ecsdo_free_slave(slave);
        return ECSDO_ERR_NO_MEMORY;
    }
    for (i = 0; i < s.sdo_count; i++) {
        ret = read_object(ctx, position, i, &slave->objects[i]);
        slave->object_count = i + 1u; /* free the partial object too */
        if (ret == -ENOMEM) {
            ecsdo_free_slave(slave);
            return ECSDO_ERR_NO_MEMORY;
        }
        if (ret) {
            /* Keep what was read; report it on the slave. */
            slave->status = ECSDO_SLAVE_ERROR;
            slave->sys_errno = -ret;
            ctx->last_errno = -ret;
            break;
        }
    }

done:
    *out = slave;
    return ECSDO_OK;
}

/* ------------------------------------------------------------------------
 * Upload
 * ---------------------------------------------------------------------- */

int ecsdo_upload(ecsdo_ctx_t *ctx, uint16_t position, uint16_t index,
        uint8_t subindex, void *buf, size_t buf_size, size_t *data_size,
        uint32_t *abort_code)
{
    ecsdo_io_slave_t s;
    uint32_t abort = 0;
    size_t size = 0;
    int ret;

    if (abort_code)
        *abort_code = 0;
    if (data_size)
        *data_size = 0;
    if (!ctx || !buf || !buf_size || !data_size)
        return ECSDO_ERR_INVALID_ARG;
    if (!ctx->writable)
        return ECSDO_ERR_NOT_WRITABLE;

    /* The upload ioctl reports a missing slave like other errors. */
    ret = ecsdo_io_slave(ctx->fd, position, &s);
    if (ret == -EINVAL)
        return ECSDO_ERR_NO_SLAVE;
    if (ret)
        return io_fail(ctx, ret);

    /* The kernel fills buf through a pointer inside the ioctl argument,
     * which memory checkers cannot see: clear it so valgrind on the
     * caller's side stays meaningful. Cheap next to the SDO transfer. */
    memset(buf, 0, buf_size);
    ret = ecsdo_io_sdo_upload(ctx->fd, position, index, subindex, buf,
            buf_size, &size, &abort);
    if (abort_code)
        *abort_code = abort;
    if (!ret) {
        *data_size = size;
        return ECSDO_OK;
    }
    if (abort)
        return ECSDO_ERR_ABORT;
    if (ret == -ENOBUFS)
        return ECSDO_ERR_BUFFER_SIZE;
    return io_fail(ctx, ret);
}

int ecsdo_upload_value(ecsdo_ctx_t *ctx, uint16_t position, uint16_t index,
        uint8_t subindex, void *buf, size_t buf_size, ecsdo_value_t *val,
        uint32_t *abort_code)
{
    ecsdo_io_sdo_entry_t io;
    size_t size;
    int ret, known;

    if (!val)
        return ECSDO_ERR_INVALID_ARG;
    memset(val, 0, sizeof(*val));
    if (!ctx)
        return ECSDO_ERR_INVALID_ARG;

    /* Type check first, like `ethercat upload`: the entry's type from the
     * master's dictionary. */
    ret = ecsdo_io_sdo_entry_by_index(ctx->fd, position, index, subindex,
            &io);
    if (ret && ret != -EINVAL)
        return io_fail(ctx, ret);
    known = !ret;

    ret = ecsdo_upload(ctx, position, index, subindex, buf, buf_size, &size,
            abort_code);
    if (ret)
        return ret;

    if (!known) {
        /* Not in the dictionary: raw bytes only. */
        val->data = buf;
        val->size = size;
        val->kind = ECSDO_VALUE_RAW;
        return ECSDO_ERR_UNKNOWN_TYPE;
    }
    ret = ecsdo_decode(io.data_type, buf, size, val);
    val->bit_length = io.bit_length;
    return ret;
}

const char *ecsdo_strerror(int code)
{
    switch (code) {
        case ECSDO_OK: return "success";
        case ECSDO_ERR_INVALID_ARG: return "invalid argument";
        case ECSDO_ERR_NO_MEMORY: return "out of memory";
        case ECSDO_ERR_OPEN: return "cannot open EtherCAT master device";
        case ECSDO_ERR_VERSION:
            return "EtherCAT master ioctl version mismatch";
        case ECSDO_ERR_IOCTL: return "EtherCAT master ioctl failed";
        case ECSDO_ERR_NO_SLAVE: return "no such slave";
        case ECSDO_ERR_TIMEOUT: return "timeout waiting for dictionaries";
        case ECSDO_ERR_NOT_WRITABLE:
            return "SDO upload needs read-write access to the master device";
        case ECSDO_ERR_ABORT: return "SDO abort from slave";
        case ECSDO_ERR_BUFFER_SIZE: return "buffer too small for the value";
        case ECSDO_ERR_TYPE_MISMATCH:
            return "value size does not match its data type";
        case ECSDO_ERR_UNKNOWN_TYPE: return "data type unknown";
        default: return "unknown error";
    }
}
