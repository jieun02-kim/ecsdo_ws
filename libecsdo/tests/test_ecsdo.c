/*
 * libecsdo v2 tests against the fake ioctl layer (no hardware).
 */

#include <errno.h>
#include <time.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo_ioctl.h"
#include "fake_ioctl.h"
#include "test_util.h"

static const uint8_t v_devtype[] = {0x92, 0x01, 0x02, 0x00};
static const uint8_t v_name[] = "L7NA004\0\0\0";   /* 11 bytes */
static const uint8_t v_count[] = {0x02};
static const uint8_t v_map1[] = {0x10, 0x00, 0x40, 0x60};

static fake_entry_t ent(uint8_t sub, uint16_t type, uint16_t bits,
        int readable, const uint8_t *value, size_t len)
{
    fake_entry_t e;

    memset(&e, 0, sizeof(e));
    e.subindex = sub;
    e.data_type = type;
    e.bit_length = bits;
    e.read_access[0] = e.read_access[1] = e.read_access[2] =
        (uint8_t) readable;
    e.write_access[0] = 1;
    e.description = "entry";
    e.value = value;
    e.value_len = len;
    return e;
}

/** CoE drive, 4 objects, fully fetched dictionary. 0x1600 has a subindex
 *  gap (no :02) and an entry that aborts (:03). */
static void make_drive(fake_slave_t *s)
{
    fake_object_t *o;

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x7595;
    s->revision = 2;
    s->serial = 7;
    s->alias = 3;
    s->mailbox_protocols = ECSDO_MBOX_COE;
    s->has_general_category = 1;
    s->enable_sdo_info = 1;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "Drive";
    s->object_count = 4;

    o = &s->objects[0];
    o->index = 0x1000; o->name = "Device Type"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0007, 32, 1, v_devtype, 4);

    o = &s->objects[1];
    o->index = 0x1008; o->name = "Device Name"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0009, 88, 1, v_name, 11);

    o = &s->objects[2];
    o->index = 0x1600; o->name = "RxPDO"; o->max_subindex = 3;
    o->entry_count = 3;
    o->entries[0] = ent(0, 0x0005, 8, 1, v_count, 1);
    o->entries[1] = ent(1, 0x0007, 32, 1, v_map1, 4);
    o->entries[2] = ent(3, 0x0007, 32, 1, NULL, 0);
    o->entries[2].abort_code = 0x06090011;

    o = &s->objects[3];
    o->index = 0x2001; o->name = "Flaky"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0005, 8, 0, NULL, 0);
    o->entries[0].upload_errno = ETIMEDOUT;

    s->visible_objects = s->filled_objects = s->object_count;
}

static void make_plain(fake_slave_t *s)
{
    memset(s, 0, sizeof(*s));
    s->vendor_id = 2;
    s->product_code = 0x044c2c52;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "EK1100";
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------ */

static void test_open(void)
{
    ecsdo_ctx_t *ctx;
    uint16_t n;

    fake_reset();
    fake.slave_count = 3;
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);
    CHECK_EQ(ecsdo_slave_count(ctx, &n), ECSDO_OK);
    CHECK_EQ(n, 3);
    ecsdo_close(ctx);

    fake.magic = 36;
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_ERR_VERSION);
    CHECK(ctx == NULL);

    fake_reset();
    fake.open_errno = ENOENT;
    CHECK_EQ(ecsdo_open(&ctx, 3), ECSDO_ERR_OPEN);
    CHECK_EQ(errno, ENOENT);

    CHECK_EQ(ecsdo_open(NULL, 0), ECSDO_ERR_INVALID_ARG);
    ecsdo_close(NULL);
}

static void test_read_dict(void)
{
    ecsdo_ctx_t *ctx;
    ecsdo_slave_t *s;
    const ecsdo_object_t *o;

    fake_reset();
    fake.slave_count = 2;
    make_drive(&fake.slaves[0]);
    make_plain(&fake.slaves[1]);
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);

    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_OK);
    CHECK_EQ(s->position, 0);
    CHECK_EQ(s->alias, 3);
    CHECK_EQ(s->vendor_id, 0x7595);
    CHECK_EQ(s->revision, 2);
    CHECK_EQ(s->serial, 7);
    CHECK_STR(s->name, "Drive");
    CHECK_EQ(s->coe, 1);
    CHECK_EQ(s->sdo_info, 1);
    CHECK_EQ(s->object_count, 4);
    o = &s->objects[2];
    CHECK_EQ(o->index, 0x1600);
    CHECK_STR(o->name, "RxPDO");
    CHECK_EQ(o->max_subindex, 3);
    CHECK_EQ(o->entry_count, 3);             /* gap at :02 skipped */
    CHECK_EQ(o->entries[2].subindex, 3);
    CHECK_EQ(o->entries[1].data_type, 0x0007);
    CHECK_EQ(o->entries[1].bit_length, 32);
    CHECK_EQ(o->entries[1].read_access[ECSDO_ACCESS_OP], 1);
    CHECK_EQ(o->entries[1].write_access[ECSDO_ACCESS_PREOP], 1);
    CHECK_EQ(o->entries[1].write_access[ECSDO_ACCESS_OP], 0);
    CHECK_STR(o->entries[1].description, "entry");
    CHECK_EQ(fake.upload_calls, 0);          /* dictionary only */
    ecsdo_free_slave(s);

    CHECK_EQ(ecsdo_read_dict(ctx, 1, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NO_COE);
    CHECK_EQ(s->product_code, 0x044c2c52);
    CHECK_EQ(s->object_count, 0);
    ecsdo_free_slave(s);

    CHECK_EQ(ecsdo_read_dict(ctx, 2, &s), ECSDO_ERR_NO_SLAVE);
    CHECK(s == NULL);
    CHECK_EQ(ecsdo_read_dict(ctx, 0, NULL), ECSDO_ERR_INVALID_ARG);
    ecsdo_free_slave(NULL);
    ecsdo_close(ctx);
}

static void test_read_dict_states(void)
{
    ecsdo_ctx_t *ctx;
    ecsdo_slave_t *s;

    fake_reset();
    fake.slave_count = 1;
    make_drive(&fake.slaves[0]);
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);

    /* No SDO information */
    fake.slaves[0].enable_sdo_info = 0;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NO_SDO_INFO);
    CHECK_EQ(s->coe, 1);
    CHECK_EQ(s->object_count, 0);
    ecsdo_free_slave(s);

    /* No general SII category: the master still uses SDO info. */
    fake.slaves[0].has_general_category = 0;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_OK);
    ecsdo_free_slave(s);
    make_drive(&fake.slaves[0]);

    /* Right after a rescan: not "no CoE", but not ready. */
    fake.slaves[0].al_state = 0;
    fake.slaves[0].mailbox_protocols = 0;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NOT_READY);
    ecsdo_free_slave(s);
    make_drive(&fake.slaves[0]);
    fake.scan_busy = 1;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NOT_READY);
    ecsdo_free_slave(s);
    fake.scan_busy = 0;

    /* List known, only 2 of 4 objects described yet. */
    fake.slaves[0].filled_objects = 2;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NOT_READY);
    CHECK_EQ(s->object_count, 4);
    CHECK_EQ(s->objects[1].entry_count, 1);
    CHECK_EQ(s->objects[3].entry_count, 0);
    ecsdo_free_slave(s);
    make_drive(&fake.slaves[0]);

    /* No dictionary at all yet. */
    fake.slaves[0].visible_objects = 0;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NOT_READY);
    CHECK_EQ(s->object_count, 0);
    ecsdo_free_slave(s);
    make_drive(&fake.slaves[0]);

    /* ioctl failure mid-dictionary: partial result, ERROR. */
    fake.slaves[0].objects[2].sdo_errno = EIO;
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_ERROR);
    CHECK_EQ(s->sys_errno, EIO);
    CHECK_EQ(s->objects[1].entry_count, 1);
    ecsdo_free_slave(s);

    ecsdo_close(ctx);
}

/* wait_ready simulation: advances on every master query. */
static unsigned int ticks;

static void progress_dictionary(void)
{
    fake_slave_t *s = &fake.slaves[0];

    ticks++;
    if (ticks < 3) {
        fake.scan_busy = 1;
        s->al_state = 0;
        s->mailbox_protocols = 0;
        return;
    }
    fake.scan_busy = 0;
    s->al_state = ECSDO_AL_PREOP;
    s->mailbox_protocols = ECSDO_MBOX_COE;
    if (ticks >= 5)
        s->visible_objects = s->object_count;
    if (ticks >= 6 && s->filled_objects < s->object_count)
        s->filled_objects++;
}

static void test_wait_ready(void)
{
    ecsdo_ctx_t *ctx;
    ecsdo_slave_t *s;
    double t0, dt;
    size_t i;

    /* Dictionary arrives while waiting. */
    fake_reset();
    fake.slave_count = 2;
    make_drive(&fake.slaves[0]);
    fake.slaves[0].visible_objects = fake.slaves[0].filled_objects = 0;
    make_plain(&fake.slaves[1]);
    ticks = 0;
    fake.on_master_poll = progress_dictionary;
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);
    CHECK_EQ(ecsdo_wait_ready(ctx, 10000), ECSDO_OK);
    CHECK_EQ(fake.slaves[0].filled_objects, fake.slaves[0].object_count);
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_OK);
    for (i = 0; i < s->object_count; i++)
        CHECK(s->objects[i].entry_count > 0);
    ecsdo_free_slave(s);
    ecsdo_close(ctx);

    /* One slave never reaches PREOP. */
    fake_reset();
    fake.slave_count = 2;
    make_drive(&fake.slaves[0]);
    make_drive(&fake.slaves[1]);
    fake.slaves[1].al_state = ECSDO_AL_INIT;
    fake.slaves[1].visible_objects = fake.slaves[1].filled_objects = 0;
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);
    t0 = now_s();
    CHECK_EQ(ecsdo_wait_ready(ctx, 300), ECSDO_ERR_TIMEOUT);
    dt = now_s() - t0;
    CHECK(dt >= 0.29 && dt < 1.0);
    CHECK_EQ(ecsdo_read_dict(ctx, 0, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_OK);
    ecsdo_free_slave(s);
    CHECK_EQ(ecsdo_read_dict(ctx, 1, &s), ECSDO_OK);
    CHECK_EQ(s->status, ECSDO_SLAVE_NOT_READY);
    ecsdo_free_slave(s);
    ecsdo_close(ctx);
}

static void test_upload(void)
{
    uint8_t buf[16];
    size_t n;
    uint32_t abort_code;
    ecsdo_ctx_t *ctx;

    fake_reset();
    fake.slave_count = 1;
    make_drive(&fake.slaves[0]);
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);

    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1000, 0, buf, sizeof(buf), &n,
                &abort_code), ECSDO_OK);
    CHECK_EQ(n, 4);
    CHECK(!memcmp(buf, v_devtype, 4));
    CHECK_EQ(abort_code, 0);
    CHECK_EQ(fake.upload_sizes[0], sizeof(buf)); /* caller's buffer */

    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1008, 0, buf, sizeof(buf), &n, NULL),
            ECSDO_OK);
    CHECK_EQ(n, 11);
    CHECK(!memcmp(buf, "L7NA004", 7));

    /* Too small: no retry, caller decides. */
    fake.upload_calls = 0;
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1008, 0, buf, 4, &n, NULL),
            ECSDO_ERR_BUFFER_SIZE);
    CHECK_EQ(n, 0);
    CHECK_EQ(fake.upload_calls, 1);

    /* Upload does not check the dictionary's read access: the slave
     * decides (here: transport error). */
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x2001, 0, buf, sizeof(buf), &n, NULL),
            ECSDO_ERR_IOCTL);
    CHECK_EQ(ecsdo_last_errno(ctx), ETIMEDOUT);

    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1600, 3, buf, sizeof(buf), &n,
                &abort_code), ECSDO_ERR_ABORT);
    CHECK_EQ(abort_code, 0x06090011);
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x7777, 0, buf, sizeof(buf), &n,
                &abort_code), ECSDO_ERR_ABORT);
    CHECK_EQ(abort_code, 0x06020000);

    /* No dictionary needed (e.g. no SDO information). */
    fake.slaves[0].visible_objects = 0;
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1000, 0, buf, sizeof(buf), &n, NULL),
            ECSDO_OK);
    CHECK_EQ(n, 4);

    CHECK_EQ(ecsdo_upload(ctx, 5, 0x1000, 0, buf, sizeof(buf), &n, NULL),
            ECSDO_ERR_NO_SLAVE);
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1000, 0, NULL, 4, &n, NULL),
            ECSDO_ERR_INVALID_ARG);
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1000, 0, buf, 0, &n, NULL),
            ECSDO_ERR_INVALID_ARG);
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1000, 0, buf, 4, NULL, NULL),
            ECSDO_ERR_INVALID_ARG);
    ecsdo_close(ctx);

    /* Read-only handle: dictionary yes, upload no. */
    fake.rw_open_errno = EACCES;
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);
    CHECK_EQ(ecsdo_upload(ctx, 0, 0x1000, 0, buf, sizeof(buf), &n, NULL),
            ECSDO_ERR_NOT_WRITABLE);
    ecsdo_close(ctx);
}

static void test_decode(void)
{
    static const uint8_t u32[] = {0x92, 0x01, 0x02, 0x00};
    static const uint8_t u24[] = {0x56, 0x34, 0x12};
    static const uint8_t i8[] = {0xfd};
    static const uint8_t i16[] = {0x48, 0xf4};
    static const uint8_t i24[] = {0xff, 0xff, 0xff};
    static const uint8_t i64[] = {0, 0, 0, 0, 0, 0, 0, 0x80};
    static const uint8_t bits[] = {0xfe};
    static const uint8_t str[] = "L7NA004\0\0\0";
    float f = -1.5f;
    double d = 3.141592653589793;
    uint8_t fb[4], db[8];
    ecsdo_value_t v;

    CHECK_STR(ecsdo_type_name(0x0007), "UNSIGNED32");
    CHECK_STR(ecsdo_type_name(0x0033), "BIT4");
    CHECK(ecsdo_type_name(0x0017) == NULL);

    CHECK_EQ(ecsdo_decode(0x0007, u32, 4, &v), ECSDO_OK);
    CHECK_EQ(v.kind, ECSDO_VALUE_UNSIGNED);
    CHECK_EQ(v.u, 0x00020192);
    CHECK_EQ(v.size, 4);
    CHECK_EQ(ecsdo_decode(0x0016, u24, 3, &v), ECSDO_OK);
    CHECK_EQ(v.u, 0x123456);

    CHECK_EQ(ecsdo_decode(0x0002, i8, 1, &v), ECSDO_OK);
    CHECK_EQ(v.kind, ECSDO_VALUE_SIGNED);
    CHECK_EQ(v.i, -3);
    CHECK_EQ(ecsdo_decode(0x0003, i16, 2, &v), ECSDO_OK);
    CHECK_EQ(v.i, -3000);
    CHECK_EQ(ecsdo_decode(0x0010, i24, 3, &v), ECSDO_OK);
    CHECK_EQ(v.i, -1);
    CHECK_EQ(ecsdo_decode(0x0015, i64, 8, &v), ECSDO_OK);
    CHECK(v.i == INT64_MIN);

    CHECK_EQ(ecsdo_decode(0x0001, bits, 1, &v), ECSDO_OK); /* BOOLEAN */
    CHECK_EQ(v.u, 0);
    CHECK_EQ(ecsdo_decode(0x0032, bits, 1, &v), ECSDO_OK); /* BIT3 */
    CHECK_EQ(v.u, 6);

    memcpy(fb, &f, 4);
    memcpy(db, &d, 8);
    CHECK_EQ(ecsdo_decode(0x0008, fb, 4, &v), ECSDO_OK);
    CHECK_EQ(v.kind, ECSDO_VALUE_REAL);
    CHECK(v.f == -1.5);
    CHECK_EQ(ecsdo_decode(0x0011, db, 8, &v), ECSDO_OK);
    CHECK(v.f == d);

    CHECK_EQ(ecsdo_decode(0x0009, str, 11, &v), ECSDO_OK);
    CHECK_EQ(v.kind, ECSDO_VALUE_STRING);
    CHECK_EQ(v.str_len, 7);
    CHECK(!memcmp(v.str, "L7NA004", 7));
    CHECK_EQ(ecsdo_decode(0x0009, str, 0, &v), ECSDO_OK);
    CHECK_EQ(v.str_len, 0);

    /* Type check: size must match the type exactly. */
    CHECK_EQ(ecsdo_decode(0x0007, u32, 3, &v), ECSDO_ERR_TYPE_MISMATCH);
    CHECK_EQ(v.kind, ECSDO_VALUE_RAW);
    CHECK_EQ(v.size, 3);
    CHECK_EQ(ecsdo_decode(0x0005, u32, 4, &v), ECSDO_ERR_TYPE_MISMATCH);

    /* Not decoded, but known: raw with OK. Unknown type: error. */
    CHECK_EQ(ecsdo_decode(0x000A, u32, 4, &v), ECSDO_OK);
    CHECK_EQ(v.kind, ECSDO_VALUE_RAW);
    CHECK_EQ(ecsdo_decode(0x0000, u32, 4, &v), ECSDO_ERR_UNKNOWN_TYPE);
    CHECK_EQ(v.kind, ECSDO_VALUE_RAW);
    CHECK(v.data == u32);
    CHECK_EQ(ecsdo_decode(0x0007, NULL, 4, &v), ECSDO_ERR_INVALID_ARG);

    CHECK_STR(ecsdo_abort_string(0x06090011), "Subindex does not exist");
    CHECK(ecsdo_abort_string(0x12345678) == NULL);
}

static void test_upload_value(void)
{
    static const uint8_t short_u32[] = {1, 2, 3};
    uint8_t buf[32];
    uint32_t abort_code;
    ecsdo_value_t v;
    ecsdo_ctx_t *ctx;

    fake_reset();
    fake.slave_count = 1;
    make_drive(&fake.slaves[0]);
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);

    CHECK_EQ(ecsdo_upload_value(ctx, 0, 0x1000, 0, buf, sizeof(buf), &v,
                NULL), ECSDO_OK);
    CHECK_EQ(v.data_type, 0x0007);
    CHECK_EQ(v.bit_length, 32);
    CHECK_EQ(v.kind, ECSDO_VALUE_UNSIGNED);
    CHECK_EQ(v.u, 0x00020192);
    CHECK(v.data == buf);

    CHECK_EQ(ecsdo_upload_value(ctx, 0, 0x1008, 0, buf, sizeof(buf), &v,
                NULL), ECSDO_OK);
    CHECK_EQ(v.kind, ECSDO_VALUE_STRING);
    CHECK_EQ(v.str_len, 7);
    CHECK_EQ(v.size, 11);

    /* Slave sends 3 bytes for an UNSIGNED32 entry. */
    fake.slaves[0].objects[0].entries[0].value = short_u32;
    fake.slaves[0].objects[0].entries[0].value_len = 3;
    CHECK_EQ(ecsdo_upload_value(ctx, 0, 0x1000, 0, buf, sizeof(buf), &v,
                NULL), ECSDO_ERR_TYPE_MISMATCH);
    CHECK_EQ(v.kind, ECSDO_VALUE_RAW);
    CHECK_EQ(v.size, 3);
    make_drive(&fake.slaves[0]);

    CHECK_EQ(ecsdo_upload_value(ctx, 0, 0x1600, 3, buf, sizeof(buf), &v,
                &abort_code), ECSDO_ERR_ABORT);
    CHECK_EQ(abort_code, 0x06090011);

    /* Not in the dictionary: transferred, but raw. */
    fake.slaves[0].visible_objects = 0;
    CHECK_EQ(ecsdo_upload_value(ctx, 0, 0x1000, 0, buf, sizeof(buf), &v,
                NULL), ECSDO_ERR_UNKNOWN_TYPE);
    CHECK_EQ(v.kind, ECSDO_VALUE_RAW);
    CHECK_EQ(v.size, 4);
    CHECK_EQ(ecsdo_decode(0x0007, v.data, v.size, &v), ECSDO_OK);
    CHECK_EQ(v.u, 0x00020192);

    CHECK_EQ(ecsdo_upload_value(ctx, 0, 0x1000, 0, buf, sizeof(buf), NULL,
                NULL), ECSDO_ERR_INVALID_ARG);
    CHECK_EQ(ecsdo_upload_value(ctx, 9, 0x1000, 0, buf, sizeof(buf), &v,
                NULL), ECSDO_ERR_NO_SLAVE);
    ecsdo_close(ctx);
}

static void test_strerror(void)
{
    int code;

    for (code = ECSDO_OK; code >= ECSDO_ERR_UNKNOWN_TYPE; code--)
        CHECK(strcmp(ecsdo_strerror(code), "unknown error") != 0);
    CHECK_STR(ecsdo_strerror(-100), "unknown error");
}

int main(void)
{
    RUN(test_open);
    RUN(test_read_dict);
    RUN(test_read_dict_states);
    RUN(test_wait_ready);
    RUN(test_upload);
    RUN(test_decode);
    RUN(test_upload_value);
    RUN(test_strerror);
    return TEST_RESULT();
}
