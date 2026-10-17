/*
 * plan_run - apply the PDO mapping plans from tools/ecat_rules
 * (rule_engine.py -> plans/slave_N.plan.json), bring the bus to OP and run a
 * small test per device class:
 *
 *   servo_drive     CiA402 enable, then a small move and back, repeated
 *                   csp: target position +amp and back (cosine profile)
 *                   csv: small velocity forward then backward
 *                   cst: short small torque pulses, position/speed guarded
 *                   then disable.
 *   digital_output  walking 1 over the channels, then all off.
 *
 * The test runs until Ctrl+C (or -t seconds). If the link goes down or the
 * set of responding slaves changes, the drives are disabled and plan_run
 * exits with 3 so that run_test.sh can probe the new bus and start over.
 *   digital_input   prints changes.
 *   others          outputs stay zero, inputs printed.
 *
 *   plan_run [options] plans/slave_*.plan.json
 *
 * Mapping: the plan's rx/tx (PDO index, entries in order) is handed to IgH
 * with ecrt_slave_config_pdos(); IgH writes 0x1C12/0x1C13/0x16xx/0x1Axx
 * itself in PREOP when the slave supports CoE (same as the plan's
 * sdo_writes). The other sdo_writes (e.g. 0x6060 mode) are queued as
 * configuration SDOs. The plan must match the bus (position, vendor,
 * product); otherwise nothing is configured.
 *
 * Safety: Controlword stays 0 (drive disabled) until all slaves are OP;
 * target position follows the actual position until the drive is enabled;
 * following error, speed and travel are checked every cycle and the drive
 * is disabled on any violation, fault, or Ctrl+C.
 */

#include <ctype.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <ecrt.h>

#define MAX_SLAVES 16
#define MAX_ENT 64
#define MAX_PDO 32
#define MAX_SDO 48
#define MAX_AXES 4
#define MAX_CH 64
#define CYCLE_NS 1000000L
#define OP_TIMEOUT_MS 15000
#define ENABLE_TIMEOUT_MS 5000
#define SPEED_WIN 10            /* speed averaged over this many ms */
#define SETTLE_MS 200           /* no speed guard right after enable */
#define RC_BUS_CHANGED 3        /* exit code: probe the bus again */

/* ===================================================================== */
/* Minimal JSON reader (enough for plan files)                           */
/* ===================================================================== */

enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ };

typedef struct jv {
    int t;
    double num;
    char *s;
    size_t n;
    char **keys;                /* JOBJ only */
    struct jv *v;
} jv_t;

static const char *jp;
static int jerr;

static void jws(void)
{
    while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t')
        jp++;
}

static void jput(char **o, size_t *n, size_t *cap, char c)
{
    if (*n + 5 > *cap) {
        *cap *= 2;
        *o = realloc(*o, *cap);
    }
    (*o)[(*n)++] = c;
}

/** String at jp (on the opening quote). \uXXXX -> UTF-8 (BMP only). */
static char *jstr(void)
{
    size_t cap = 32, n = 0;
    char *o = malloc(cap);

    jp++;
    while (*jp && *jp != '"') {
        char c = *jp++;

        if (c == '\\' && *jp) {
            c = *jp++;
            if (c == 'u') {
                unsigned int u = 0;
                int i;

                for (i = 0; i < 4 && isxdigit((unsigned char) *jp); i++, jp++)
                    u = u * 16 + (unsigned int) (isdigit((unsigned char) *jp)
                            ? *jp - '0' : (tolower((unsigned char) *jp) - 'a' + 10));
                if (u < 0x80) {
                    jput(&o, &n, &cap, (char) u);
                } else if (u < 0x800) {
                    jput(&o, &n, &cap, (char) (0xC0 | (u >> 6)));
                    jput(&o, &n, &cap, (char) (0x80 | (u & 0x3F)));
                } else {
                    jput(&o, &n, &cap, (char) (0xE0 | (u >> 12)));
                    jput(&o, &n, &cap, (char) (0x80 | ((u >> 6) & 0x3F)));
                    jput(&o, &n, &cap, (char) (0x80 | (u & 0x3F)));
                }
                continue;
            }
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r'
                : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
        }
        jput(&o, &n, &cap, c);
    }
    if (*jp == '"')
        jp++;
    else
        jerr = 1;
    o[n] = '\0';
    return o;
}

static void jparse(jv_t *v)
{
    jws();
    memset(v, 0, sizeof(*v));
    if (*jp == '{' || *jp == '[') {
        int obj = *jp == '{';
        char close = obj ? '}' : ']';

        v->t = obj ? JOBJ : JARR;
        jp++;
        jws();
        if (*jp == close) {
            jp++;
            return;
        }
        for (;;) {
            jws();
            v->v = realloc(v->v, (v->n + 1) * sizeof(*v->v));
            if (obj) {
                v->keys = realloc(v->keys, (v->n + 1) * sizeof(*v->keys));
                if (*jp != '"') {
                    jerr = 1;
                    return;
                }
                v->keys[v->n] = jstr();
                jws();
                if (*jp != ':') {
                    free(v->keys[v->n]);
                    jerr = 1;
                    return;
                }
                jp++;
            }
            jparse(&v->v[v->n++]);
            if (jerr)
                return;
            jws();
            if (*jp == ',') {
                jp++;
                continue;
            }
            if (*jp == close) {
                jp++;
                return;
            }
            jerr = 1;
            return;
        }
    } else if (*jp == '"') {
        v->t = JSTR;
        v->s = jstr();
    } else if (!strncmp(jp, "true", 4) || !strncmp(jp, "false", 5)) {
        v->t = JBOOL;
        v->num = *jp == 't';
        jp += *jp == 't' ? 4 : 5;
    } else if (!strncmp(jp, "null", 4)) {
        v->t = JNULL;
        jp += 4;
    } else {
        char *end;

        v->t = JNUM;
        v->num = strtod(jp, &end);
        if (end == jp)
            jerr = 1;
        jp = end;
    }
}

static void jfree(jv_t *v)
{
    size_t i;

    for (i = 0; i < v->n; i++) {
        jfree(&v->v[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->v);
    free(v->keys);
    free(v->s);
}

static const jv_t *jget(const jv_t *o, const char *key)
{
    size_t i;

    if (!o || o->t != JOBJ)
        return NULL;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->keys[i], key))
            return &o->v[i];
    return NULL;
}

/** Number or numeric string ("0x6040", "12"). */
static int jnum(const jv_t *v, unsigned long *out)
{
    char *end;

    if (!v)
        return 0;
    if (v->t == JNUM) {
        *out = (unsigned long) (long) v->num;
        return 1;
    }
    if (v->t == JSTR && v->s[0]) {
        *out = strtoul(v->s, &end, 0);
        return *end == '\0';
    }
    return 0;
}

static void jcopy(const jv_t *v, char *dst, size_t size)
{
    snprintf(dst, size, "%s", v && v->t == JSTR ? v->s : "");
}

/* ===================================================================== */
/* Plan -> device                                                        */
/* ===================================================================== */

typedef struct {
    uint16_t index, pdo;
    uint8_t sub, bits;
    int out;                    /* 1 = RxPDO (master -> slave) */
    char role[48];
    int off;                    /* domain byte offset, -1 = not registered */
    unsigned int bit;
} ent_t;

typedef struct {
    uint16_t index;
    uint8_t sub, bits;
    uint32_t value;
    char note[64];
} sdo_t;

enum { PH_WAIT, PH_ENABLE, PH_HOLD, PH_MOVE, PH_HOLD2, PH_DISABLE, PH_DONE };
static const char *ph_name[] = {
    "wait", "enable", "hold", "move", "hold", "disable", "done"
};

typedef struct {
    int cw, sw, tpos, tvel, ttq, apos, avel, atq;   /* entry ids, -1 none */
    int phase, t;                                   /* ms in phase */
    int32_t hold, cmd_pos, peak;
    int moves;
    int32_t hist[SPEED_WIN];                        /* past positions */
    int hi;
    uint16_t last_sw;
    int resets;
} axis_t;

enum { K_NONE, K_DRIVE, K_IO };

typedef struct {
    char path[256];
    int pos, kind, count, mode;     /* mode: 8 csp, 9 csv, 10 cst */
    uint32_t vid, pid;
    char name[64], cls[32], rule[48], action[24], status[24];
    ent_t e[MAX_ENT];
    int ne;
    sdo_t w[MAX_SDO];
    int nw;
    /* IgH configuration built from e[] */
    ec_pdo_entry_info_t ie[MAX_ENT];
    ec_pdo_info_t ip[MAX_PDO];
    ec_sync_info_t is[2];
    int nsync;
    ec_slave_config_t *sc;
    axis_t ax[MAX_AXES];
    int nax;
    int dout[MAX_CH], ndout, din[MAX_CH], ndin;
    int has_tq_limit;
    uint16_t tq_limit_orig;
} node_t;

static int load_entries(node_t *d, const jv_t *arr, int out)
{
    size_t i;

    if (!arr || arr->t != JARR)
        return 0;
    for (i = 0; i < arr->n; i++) {
        const jv_t *o = &arr->v[i];
        unsigned long idx, sub, bits, pdo;
        ent_t *e;

        if (d->ne >= MAX_ENT) {
            fprintf(stderr, "%s: too many entries\n", d->path);
            return -1;
        }
        if (!jnum(jget(o, "index"), &idx) || !jnum(jget(o, "bit_len"), &bits)
                || !jnum(jget(o, "pdo"), &pdo)) {
            fprintf(stderr, "%s: bad PDO entry %zu\n", d->path, i);
            return -1;
        }
        if (!jnum(jget(o, "subindex"), &sub))
            sub = 0;
        e = &d->e[d->ne++];
        e->index = (uint16_t) idx;
        e->sub = (uint8_t) sub;
        e->bits = (uint8_t) bits;
        e->pdo = (uint16_t) pdo;
        e->out = out;
        e->off = -1;
        jcopy(jget(o, "role"), e->role, sizeof(e->role));
    }
    return 0;
}

static int load_plan(const char *path, node_t *d)
{
    const jv_t *id, *ws;
    unsigned long v;
    jv_t root;
    char *buf;
    long size;
    size_t i;
    FILE *f = fopen(path, "rb");
    int rc = -1;

    memset(d, 0, sizeof(*d));
    snprintf(d->path, sizeof(d->path), "%s", path);
    if (!f) {
        perror(path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    buf = malloc((size_t) size + 1);
    if (fread(buf, 1, (size_t) size, f) != (size_t) size) {
        perror(path);
        fclose(f);
        free(buf);
        return -1;
    }
    buf[size] = '\0';
    fclose(f);

    jp = buf;
    jerr = 0;
    jparse(&root);
    if (jerr || root.t != JOBJ) {
        fprintf(stderr, "%s: JSON parse error near byte %ld\n", path,
                (long) (jp - buf));
        goto out;
    }
    id = jget(&root, "identity");
    if (!jnum(jget(&root, "position"), &v))
        goto bad;
    d->pos = (int) v;
    if (!jnum(jget(id, "vendor_id"), &v))
        goto bad;
    d->vid = (uint32_t) v;
    if (!jnum(jget(id, "product_code"), &v))
        goto bad;
    d->pid = (uint32_t) v;
    jcopy(jget(id, "device_name"), d->name, sizeof(d->name));
    jcopy(jget(&root, "device_class"), d->cls, sizeof(d->cls));
    jcopy(jget(&root, "rule"), d->rule, sizeof(d->rule));
    jcopy(jget(&root, "action"), d->action, sizeof(d->action));
    jcopy(jget(&root, "status"), d->status, sizeof(d->status));
    d->count = jnum(jget(&root, "count"), &v) ? (int) v : 1;

    if (load_entries(d, jget(&root, "rx"), 1)
            || load_entries(d, jget(&root, "tx"), 0))
        goto out;

    ws = jget(&root, "sdo_writes");
    for (i = 0; ws && ws->t == JARR && i < ws->n; i++) {
        const jv_t *o = &ws->v[i];
        unsigned long idx, sub, bits, val;
        sdo_t *w;

        if (d->nw >= MAX_SDO)
            break;
        if (!jnum(jget(o, "index"), &idx) || !jnum(jget(o, "subindex"), &sub)
                || !jnum(jget(o, "bits"), &bits)
                || !jnum(jget(o, "value"), &val))
            goto bad;
        w = &d->w[d->nw++];
        w->index = (uint16_t) idx;
        w->sub = (uint8_t) sub;
        w->bits = (uint8_t) bits;
        w->value = (uint32_t) val;
        jcopy(jget(o, "note"), w->note, sizeof(w->note));
        if (idx == 0x6060)
            d->mode = (int) (int8_t) val;
    }
    rc = 0;
    goto out;
bad:
    fprintf(stderr, "%s: missing or bad field\n", path);
out:
    jfree(&root);
    free(buf);
    return rc;
}

/** Entry id with role "base" (single axis) or "base[k]". */
static int find_role(const node_t *d, const char *base, int k)
{
    char want[64];
    int i;

    snprintf(want, sizeof(want), "%s[%d]", base, k);
    for (i = 0; i < d->ne; i++)
        if ((k == 0 && !strcmp(d->e[i].role, base))
                || !strcmp(d->e[i].role, want))
            return i;
    return -1;
}

/** Work out what to do with the device from class and roles. */
static void classify(node_t *d)
{
    const char *m = strrchr(d->rule, '/');
    int i, k;

    if (m && !d->mode)
        d->mode = !strcmp(m + 1, "csp") ? 8 : !strcmp(m + 1, "csv") ? 9
            : !strcmp(m + 1, "cst") ? 10 : 0;

    if (!strcmp(d->cls, "servo_drive")) {
        d->kind = K_DRIVE;
        for (k = 0; k < d->count && k < MAX_AXES; k++) {
            axis_t *a = &d->ax[k];

            a->cw = find_role(d, "control_word", k);
            a->sw = find_role(d, "status_word", k);
            a->tpos = find_role(d, "target_position", k);
            a->tvel = find_role(d, "target_velocity", k);
            a->ttq = find_role(d, "target_torque", k);
            a->apos = find_role(d, "actual_position", k);
            a->avel = find_role(d, "actual_velocity", k);
            a->atq = find_role(d, "actual_torque", k);
            if (a->cw < 0 || a->sw < 0)
                break;
            d->nax++;
        }
        return;
    }
    for (i = 0; i < d->ne; i++) {
        if (!strncmp(d->e[i].role, "digital_output", 14) && d->ndout < MAX_CH)
            d->dout[d->ndout++] = i;
        else if (!strncmp(d->e[i].role, "digital_input", 13)
                && d->ndin < MAX_CH)
            d->din[d->ndin++] = i;
    }
    d->kind = d->ne ? K_IO : K_NONE;
}

/** Plan entries -> ec_sync_info_t (PDOs in plan order, entries in order). */
static int build_syncs(node_t *d, int sm_out, int sm_in)
{
    int dir, i, j, ne = 0, np = 0;

    d->nsync = 0;
    for (dir = 1; dir >= 0; dir--) {
        int first = np;

        for (i = 0; i < d->ne; i++) {
            if (d->e[i].out != dir)
                continue;
            for (j = first; j < np; j++)
                if (d->ip[j].index == d->e[i].pdo)
                    break;
            if (j < np)
                continue;
            if (np >= MAX_PDO)
                return -1;
            d->ip[np].index = d->e[i].pdo;
            d->ip[np].entries = &d->ie[ne];
            d->ip[np].n_entries = 0;
            for (j = i; j < d->ne; j++) {
                if (d->e[j].out != dir || d->e[j].pdo != d->e[i].pdo)
                    continue;
                d->ie[ne].index = d->e[j].index;
                d->ie[ne].subindex = d->e[j].sub;
                d->ie[ne].bit_length = d->e[j].bits;
                ne++;
                d->ip[np].n_entries++;
            }
            np++;
        }
        if (np > first) {
            ec_sync_info_t *s = &d->is[d->nsync++];

            s->index = (uint8_t) (dir ? sm_out : sm_in);
            s->dir = dir ? EC_DIR_OUTPUT : EC_DIR_INPUT;
            s->n_pdos = (unsigned int) (np - first);
            s->pdos = &d->ip[first];
            s->watchdog_mode = EC_WD_DEFAULT;
        }
    }
    return 0;
}

/** PDO configuration objects are written by IgH from the PDO config. */
static int is_pdo_object(uint16_t index)
{
    return (index >= 0x1600 && index < 0x1800)
        || (index >= 0x1A00 && index < 0x1C00)
        || (index >= 0x1C10 && index < 0x1C30);
}

static const char *mode_name(int mode)
{
    return mode == 8 ? "CSP" : mode == 9 ? "CSV" : mode == 10 ? "CST" : "?";
}

/* ===================================================================== */
/* Process data access                                                   */
/* ===================================================================== */

static uint8_t *pd;

static int32_t rd(const node_t *d, int id)
{
    const ent_t *e;
    const uint8_t *p;

    if (id < 0 || d->e[id].off < 0)
        return 0;
    e = &d->e[id];
    p = pd + e->off;
    switch (e->bits) {
    case 1:  return EC_READ_BIT(p, e->bit);
    case 8:  return EC_READ_S8(p);
    case 16: return EC_READ_S16(p);
    case 32: return EC_READ_S32(p);
    default: return 0;
    }
}

static void wr(const node_t *d, int id, int32_t v)
{
    const ent_t *e;
    uint8_t *p;

    if (id < 0 || d->e[id].off < 0)
        return;
    e = &d->e[id];
    p = pd + e->off;
    switch (e->bits) {
    case 1:  EC_WRITE_BIT(p, e->bit, v ? 1 : 0); break;
    case 8:  EC_WRITE_S8(p, (int8_t) v); break;
    case 16: EC_WRITE_S16(p, (int16_t) v); break;
    case 32: EC_WRITE_S32(p, v); break;
    default: break;
    }
}

/* ===================================================================== */
/* Test behaviour                                                        */
/* ===================================================================== */

typedef struct {
    int motion;          /* 0 = enable/hold only */
    int amp;             /* csp: counts */
    int vel;             /* csv: counts/s */
    int torque;          /* cst: 0.1 % of rated */
    int move_ms;         /* one direction */
    int limit_ms;        /* total test time, 0 = until Ctrl+C */
    int guard;           /* max travel / following error, counts */
    int speed_guard;     /* counts per ms, 10 ms average; 0 = auto */
} opts_t;

static opts_t opt = {
    .motion = 1, .amp = 200000, .vel = 5000, .torque = 10, .move_ms = 2500,
    .limit_ms = 0, .guard = 0, .speed_guard = 0,
};

static volatile sig_atomic_t stop;   /* Ctrl+C */
static int halt;                     /* Ctrl+C or bus lost: wind down */
static int run_ms;                   /* ms since all slaves OP */

static int time_up(void)
{
    return opt.limit_ms && run_ms >= opt.limit_ms;
}

static void on_signal(int sig)
{
    (void) sig;
    if (stop)
        _exit(1);        /* second Ctrl+C: hard exit */
    stop = 1;
}

static void set_phase(const node_t *d, axis_t *a, int ph, const char *why)
{
    printf("    slave %d axis %d: %s -> %s%s%s\n", d->pos, (int) (a - d->ax),
            ph_name[a->phase], ph_name[ph], why ? "  (" : "", why ? why : "");
    a->phase = ph;
    a->t = 0;
}

/** Motion offset / command for the MOVE phase. Returns 0 when finished. */
static int profile(const node_t *d, int t, int32_t *pos_off, int32_t *vel,
        int16_t *tq)
{
    int T = opt.move_ms, D = 300;

    *pos_off = 0;
    *vel = 0;
    *tq = 0;
    switch (d->mode) {
    case 8:  /* out, dwell, back */
        if (t < T)
            *pos_off = (int32_t) (opt.amp * (1 - cos(M_PI * t / T)) / 2);
        else if (t < T + D)
            *pos_off = opt.amp;
        else if (t < 2 * T + D)
            *pos_off = (int32_t) (opt.amp * (1 + cos(M_PI * (t - T - D) / T))
                    / 2);
        else
            return 0;
        return 1;
    case 9:  /* half sine forward, half sine backward */
        if (t >= 2 * T)
            return 0;
        *vel = (int32_t) (opt.vel * sin(M_PI * (t % T) / T))
            * (t < T ? 1 : -1);
        return 1;
    case 10: /* +pulse, pause, -pulse, pause (100 ms each) */
        if (t >= 400)
            return 0;
        *tq = (int16_t) (t < 100 ? opt.torque : (t >= 200 && t < 300)
                ? -opt.torque : 0);
        return 1;
    default:
        return 0;
    }
}

static void drive_cycle(node_t *d, axis_t *a)
{
    uint16_t sw = (uint16_t) rd(d, a->sw), cw = 0;
    int32_t pos = rd(d, a->apos), off, vel = 0, speed;
    int16_t tq = 0;
    int enabled = (sw & 0x6F) == 0x27, fault = (sw & 0x4F) == 0x08;
    int guard = opt.guard ? opt.guard : 2 * opt.amp;

    /* average over SPEED_WIN ms: single-cycle steps (enable transient,
     * unsynchronised sampling) are not a runaway */
    speed = (pos - a->hist[a->hi]) / SPEED_WIN;
    a->hist[a->hi] = pos;
    a->hi = (a->hi + 1) % SPEED_WIN;
    if (sw != a->last_sw && a->phase != PH_WAIT)
        printf("    slave %d axis %d: statusword 0x%04x\n", d->pos,
                (int) (a - d->ax), sw);
    a->last_sw = sw;
    a->t++;

    if (halt && a->phase >= PH_ENABLE && a->phase <= PH_HOLD2)
        set_phase(d, a, PH_DISABLE, stop ? "stop" : "bus lost");
    if (a->phase >= PH_HOLD && a->phase <= PH_HOLD2) {
        if (fault)
            set_phase(d, a, PH_DISABLE, "fault");
        else if (!enabled)
            set_phase(d, a, PH_DISABLE, "operation enabled lost");
        else if (a->apos >= 0 && abs(pos - a->hold) > guard)
            set_phase(d, a, PH_DISABLE, "travel guard");
        else if (a->apos >= 0 && abs(speed) > opt.speed_guard
                && !(a->phase == PH_HOLD && a->t < SETTLE_MS)) {
            char why[64];

            snprintf(why, sizeof(why), "speed guard: %d > %d counts/ms",
                    abs(speed), opt.speed_guard);
            set_phase(d, a, PH_DISABLE, why);
        }
        else if (d->mode == 8 && abs(pos - a->cmd_pos) > guard)
            set_phase(d, a, PH_DISABLE, "following error");
    }

    a->cmd_pos = pos;            /* default: follow actual (no jump) */
    switch (a->phase) {
    case PH_WAIT:
    case PH_DONE:
        break;
    case PH_ENABLE:
        /* CiA402 state machine up to Operation enabled */
        if (fault) {
            if (a->resets >= 3) {
                set_phase(d, a, PH_DISABLE, "fault not cleared");
                break;
            }
            cw = (a->t % 200) < 100 ? 0x80 : 0x00;     /* reset edge */
            if (a->t % 200 == 199)
                a->resets++;
        } else if ((sw & 0x4F) == 0x40) {
            cw = 0x06;                                  /* shutdown */
        } else if ((sw & 0x6F) == 0x21) {
            cw = 0x07;                                  /* switch on */
        } else if ((sw & 0x6F) == 0x23) {
            cw = 0x0F;                                  /* enable op */
        } else if (enabled) {
            cw = 0x0F;
            a->hold = pos;
            set_phase(d, a, PH_HOLD, "operation enabled");
        } else {
            cw = 0x06;
        }
        if (a->t > ENABLE_TIMEOUT_MS)
            set_phase(d, a, PH_DISABLE, "enable timeout");
        break;
    case PH_HOLD:
    case PH_HOLD2:
        cw = 0x0F;
        a->cmd_pos = a->hold;
        if (a->t < 500)
            break;
        if (time_up())
            set_phase(d, a, PH_DISABLE, "time limit");
        else if (opt.motion) {
            a->peak = 0;
            a->moves++;
            set_phase(d, a, PH_MOVE, mode_name(d->mode));
        }
        break;                      /* --no-motion: hold until Ctrl+C */
    case PH_MOVE:
        cw = 0x0F;
        if (abs(pos - a->hold) > a->peak)
            a->peak = abs(pos - a->hold);
        if (!profile(d, a->t, &off, &vel, &tq)) {
            a->cmd_pos = a->hold;
            printf("    slave %d axis %d: move #%d peak travel %d counts, now "
                    "%d from start\n", d->pos, (int) (a - d->ax), a->moves,
                    a->peak, pos - a->hold);
            set_phase(d, a, PH_HOLD2, NULL);
            break;
        }
        a->cmd_pos = a->hold + off;
        break;
    case PH_DISABLE:
        cw = a->t < 100 ? 0x06 : 0x00;
        if (a->t >= 200)
            set_phase(d, a, PH_DONE, NULL);
        break;
    }

    wr(d, a->cw, cw);
    wr(d, a->tpos, a->cmd_pos);
    wr(d, a->tvel, a->phase == PH_MOVE ? vel : 0);
    wr(d, a->ttq, a->phase == PH_MOVE ? tq : 0);
}

static void io_cycle(node_t *d, int running)
{
    int k, step = run_ms / 300;

    for (k = 0; k < d->ndout; k++)
        wr(d, d->dout[k], running && !time_up() && !halt
                && k == step % d->ndout);
}

static void print_status(node_t *devs, int n)
{
    int i, k;

    for (i = 0; i < n; i++) {
        node_t *d = &devs[i];

        if (d->kind == K_DRIVE) {
            for (k = 0; k < d->nax; k++) {
                axis_t *a = &d->ax[k];

                printf("    S%d.%d %-7s sw=0x%04x pos=%d", d->pos, k,
                        ph_name[a->phase], (uint16_t) rd(d, a->sw),
                        rd(d, a->apos));
                if (a->avel >= 0)
                    printf(" vel=%d", rd(d, a->avel));
                if (a->atq >= 0)
                    printf(" tq=%d", rd(d, a->atq));
                printf("\n");
            }
        } else if (d->kind == K_IO) {
            printf("    S%d", d->pos);
            if (d->ndout) {
                printf(" DO:");
                for (k = 0; k < d->ndout; k++)
                    printf("%d", rd(d, d->dout[k]));
            }
            if (d->ndin) {
                printf(" DI:");
                for (k = 0; k < d->ndin; k++)
                    printf("%d", rd(d, d->din[k]));
            }
            for (k = 0; k < d->ne; k++)
                if (!d->e[k].out && d->e[k].bits > 1 && d->e[k].off >= 0)
                    printf(" %s=%d", d->e[k].role[0] ? d->e[k].role : "?",
                            rd(d, k));
            printf("\n");
        }
    }
}

/* ===================================================================== */
/* Setup                                                                 */
/* ===================================================================== */

static void print_device(const node_t *d)
{
    int s, p, e, i;

    printf("  slave %d \"%s\" 0x%08x:0x%08x  %s, rule %s, action %s\n",
            d->pos, d->name, d->vid, d->pid, d->cls, d->rule, d->action);
    for (s = 0; s < d->nsync; s++) {
        const ec_sync_info_t *y = &d->is[s];

        for (p = 0; p < (int) y->n_pdos; p++) {
            printf("    SM%u %s PDO 0x%04X:", y->index,
                    y->dir == EC_DIR_OUTPUT ? "out" : "in ", y->pdos[p].index);
            for (e = 0; e < (int) y->pdos[p].n_entries; e++)
                printf(" %04X:%02X/%u", y->pdos[p].entries[e].index,
                        y->pdos[p].entries[e].subindex,
                        y->pdos[p].entries[e].bit_length);
            printf("\n");
        }
    }
    for (i = 0; i < d->nw; i++)
        if (!is_pdo_object(d->w[i].index))
            printf("    config SDO 0x%04X:%02X = %u (%u bit)  %s\n",
                    d->w[i].index, d->w[i].sub, d->w[i].value, d->w[i].bits,
                    d->w[i].note);
    if (d->kind == K_DRIVE) {
        printf("    test: %d axis, %s, ", d->nax, mode_name(d->mode));
        if (!opt.motion)
            printf("enable and hold only\n");
        else if (d->mode == 8)
            printf("move +%d counts and back (%d ms each way)\n", opt.amp,
                    opt.move_ms);
        else if (d->mode == 9)
            printf("velocity +-%d counts/s (%d ms each way)\n", opt.vel,
                    opt.move_ms);
        else if (d->mode == 10)
            printf("torque pulses +-%d (0.1%%), 100 ms\n", opt.torque);
        else
            printf("unknown mode, enable and hold only\n");
    } else if (d->kind == K_IO) {
        printf("    test: %d DO (walking 1, 300 ms), %d DI\n", d->ndout,
                d->ndin);
    }
}

/** SM for a direction from the master's scan: the one that already has
 *  PDOs, else the first process data SM of that direction. */
static int pick_sm(ec_master_t *m, const ec_slave_info_t *si,
        ec_direction_t dir, int fallback)
{
    ec_sync_info_t sync;
    unsigned int sm;

    for (sm = 0; sm < si->sync_count; sm++)
        if (!ecrt_master_get_sync_manager(m, si->position, (uint8_t) sm,
                    &sync) && sync.dir == dir && sync.n_pdos)
            return (int) sm;
    for (sm = 2; sm < si->sync_count; sm++)
        if (!ecrt_master_get_sync_manager(m, si->position, (uint8_t) sm,
                    &sync) && sync.dir == dir)
            return (int) sm;
    return fallback;
}

static int cmp_pos(const void *a, const void *b)
{
    return ((const node_t *) a)->pos - ((const node_t *) b)->pos;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options] plans/slave_*.plan.json\n"
        "  -n, --dry-run      parse plans and print configuration only\n"
        "  --no-motion        enable drives and hold, no movement\n"
        "  -t SEC             stop the test after SEC (default: Ctrl+C)\n"
        "  --amp COUNTS       csp move distance (default 200000)\n"
        "  --vel COUNTS_S     csv peak velocity (default 5000)\n"
        "  --torque PERMILLE  cst torque, 0.1%% of rated (default 10, max 50)\n"
        "  --move-ms MS       duration of one move direction (default 2500)\n"
        "  --guard COUNTS     max travel / following error (default 2*amp)\n"
        "  --speed-guard N    max counts per ms, 10 ms average\n"
        "                     (default 3 x planned peak, at least 60)\n"
        "  --dc               drives with distributed clocks (SYNC0 1 ms)\n"
        "  --dc-assign HEX    DC AssignActivate with --dc (default 0x0300)\n",
        prog);
}

int main(int argc, char **argv)
{
    static node_t devs[MAX_SLAVES];
    int n = 0, dry = 0, dc = 0, i, k, a, rc = 1, op_timeout;
    int started = 0, t_wait = 0, t_after = -1, lost = 0, gone = 0;
    int bus_lost = 0;
    ec_master_state_t ms;
    long lat, lat_max = 0, lat_win = 0;
    unsigned int dc_assign = 0x0300;
    ec_master_t *master = NULL;
    ec_domain_t *domain = NULL;
    ec_master_info_t mi;
    struct timespec t;

    for (a = 1; a < argc; a++) {
        const char *s = argv[a], *v = a + 1 < argc ? argv[a + 1] : NULL;

        if (!strcmp(s, "-n") || !strcmp(s, "--dry-run"))
            dry = 1;
        else if (!strcmp(s, "-y") || !strcmp(s, "--yes"))
            ;                   /* old option, no prompt any more */
        else if (!strcmp(s, "--no-motion"))
            opt.motion = 0;
        else if (!strcmp(s, "--dc"))
            dc = 1;
        else if (!strcmp(s, "--no-dc"))
            dc = 0;
        else if (!strcmp(s, "-t") && v && ++a)
            opt.limit_ms = atoi(v) * 1000;
        else if (!strcmp(s, "--amp") && v && ++a)
            opt.amp = abs(atoi(v));
        else if (!strcmp(s, "--vel") && v && ++a)
            opt.vel = abs(atoi(v));
        else if (!strcmp(s, "--torque") && v && ++a)
            opt.torque = abs(atoi(v));
        else if (!strcmp(s, "--move-ms") && v && ++a)
            opt.move_ms = atoi(v);
        else if (!strcmp(s, "--guard") && v && ++a)
            opt.guard = abs(atoi(v));
        else if (!strcmp(s, "--speed-guard") && v && ++a)
            opt.speed_guard = abs(atoi(v));
        else if (!strcmp(s, "--dc-assign") && v && ++a)
            dc_assign = (unsigned int) strtoul(v, NULL, 0);
        else if (s[0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (n < MAX_SLAVES) {
            if (load_plan(s, &devs[n]))
                return 1;
            n++;
        }
    }
    if (!n) {
        usage(argv[0]);
        return 2;
    }
    if (opt.torque > 50) {
        fprintf(stderr, "--torque above 50 (5%%) refused\n");
        return 2;
    }
    if (opt.move_ms < 200)
        opt.move_ms = 200;
    if (!opt.speed_guard) {
        /* 3 x the fastest planned speed, at least 60 counts/ms */
        double peak = M_PI * opt.amp / (2.0 * opt.move_ms);

        if (opt.vel / 1000.0 > peak)
            peak = opt.vel / 1000.0;
        opt.speed_guard = 3 * peak > 60 ? (int) (3 * peak) : 60;
    }
    qsort(devs, (size_t) n, sizeof(devs[0]), cmp_pos);
    for (i = 0; i < n; i++) {
        classify(&devs[i]);
        if (i != devs[i].pos) {
            fprintf(stderr, "plans must cover positions 0..%d (missing %d)\n",
                    n - 1, i);
            return 1;
        }
        if (!strcmp(devs[i].status, "failed")
                || (devs[i].kind == K_DRIVE && devs[i].nax == 0)) {
            fprintf(stderr, "slave %d: plan status %s, no usable mapping "
                    "(see llm_step.py / rules)\n", i, devs[i].status);
            return 1;
        }
        if (strcmp(devs[i].status, "matched")
                && strcmp(devs[i].status, "no_process_data")) {
            printf("slave %d: plan status %s -> outputs stay zero\n", i,
                    devs[i].status);
            devs[i].kind = devs[i].ne ? K_IO : K_NONE;
            devs[i].ndout = 0;
        }
    }

    if (dry) {
        printf("[dry-run] configuration from %d plan(s) "
                "(SM numbers come from the bus at run time)\n", n);
        for (i = 0; i < n; i++) {
            build_syncs(&devs[i], 2, 3);
            print_device(&devs[i]);
        }
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ---- 1. master, bus must match the plans ------------------------- */
    master = ecrt_request_master(0);
    if (!master) {
        fprintf(stderr, "ecrt_request_master failed (master in use?)\n");
        return 1;
    }
    for (k = 0; k < 100; k++) {
        if (ecrt_master(master, &mi))
            goto out;
        if (!mi.scan_busy)
            break;
        usleep(100000);
    }
    if (mi.slave_count != (unsigned int) n) {
        fprintf(stderr, "bus has %u slave(s), plans for %d: rerun step 1\n",
                mi.slave_count, n);
        rc = RC_BUS_CHANGED;
        goto out;
    }
    printf("[1] bus matches plans\n");
    for (i = 0; i < n; i++) {
        node_t *d = &devs[i];
        ec_slave_info_t si;

        if (ecrt_master_get_slave(master, (uint16_t) i, &si))
            goto out;
        if (si.vendor_id != d->vid || si.product_code != d->pid) {
            fprintf(stderr, "slave %d is 0x%08x:0x%08x, plan says "
                    "0x%08x:0x%08x: rerun step 1\n", i, si.vendor_id,
                    si.product_code, d->vid, d->pid);
            rc = RC_BUS_CHANGED;
            goto out;
        }
        if (build_syncs(d, pick_sm(master, &si, EC_DIR_OUTPUT, 2),
                    pick_sm(master, &si, EC_DIR_INPUT, 3))) {
            fprintf(stderr, "slave %d: too many PDOs\n", i);
            goto out;
        }
        print_device(d);
    }

    /* cst: limit Maximum Torque (0x6072) for the test, restored at exit */
    for (i = 0; i < n; i++) {
        node_t *d = &devs[i];
        uint8_t buf[2];
        size_t rs;
        uint32_t abort_code;

        if (d->kind != K_DRIVE || d->mode != 10)
            continue;
        if (!ecrt_master_sdo_upload(master, (uint16_t) i, 0x6072, 0, buf,
                    sizeof(buf), &rs, &abort_code) && rs == 2) {
            d->has_tq_limit = 1;
            d->tq_limit_orig = EC_READ_U16(buf);
        }
    }

    /* ---- 3. configuration ---------------------------------------------- */
    printf("\n[2] configure (PDO mapping from plans)\n");
    domain = ecrt_master_create_domain(master);
    if (!domain)
        goto out;
    for (i = 0; i < n; i++) {
        node_t *d = &devs[i];
        int w;

        d->sc = ecrt_master_slave_config(master, 0, (uint16_t) i, d->vid,
                d->pid);
        if (!d->sc)
            goto out;
        if (d->nsync && ecrt_slave_config_pdos(d->sc, (unsigned int) d->nsync,
                    d->is)) {
            fprintf(stderr, "slave %d: PDO config failed\n", i);
            goto out;
        }
        for (w = 0; w < d->nw; w++) {
            sdo_t *s = &d->w[w];
            int r;

            if (is_pdo_object(s->index))
                continue;
            r = s->bits == 8 ? ecrt_slave_config_sdo8(d->sc, s->index, s->sub,
                        (uint8_t) s->value)
                : s->bits == 16 ? ecrt_slave_config_sdo16(d->sc, s->index,
                        s->sub, (uint16_t) s->value)
                : ecrt_slave_config_sdo32(d->sc, s->index, s->sub, s->value);
            if (r) {
                fprintf(stderr, "slave %d: config SDO 0x%04X failed\n", i,
                        s->index);
                goto out;
            }
        }
        if (d->has_tq_limit) {
            uint16_t lim = (uint16_t) (2 * opt.torque);

            ecrt_slave_config_sdo16(d->sc, 0x6072, 0, lim);
            printf("    slave %d: Maximum Torque 0x6072 %u -> %u for the "
                    "test\n", i, d->tq_limit_orig, lim);
        }
        if (d->kind == K_DRIVE && dc) {
            ecrt_slave_config_dc(d->sc, (uint16_t) dc_assign, CYCLE_NS, 0, 0,
                    0);
            printf("    slave %d: DC sync0 1 ms (AssignActivate 0x%04x)\n", i,
                    dc_assign);
        }
        for (k = 0; k < d->ne; k++) {
            ent_t *e = &d->e[k];

            if (!e->index)
                continue;           /* gap */
            e->off = ecrt_slave_config_reg_pdo_entry(d->sc, e->index, e->sub,
                    domain, &e->bit);
            if (e->off < 0) {
                fprintf(stderr, "slave %d: register 0x%04X:%02X failed\n", i,
                        e->index, e->sub);
                goto out;
            }
        }
    }

    /* ---- 4. OP and test ------------------------------------------------- */
    printf("\n[3] activate, wait for OP (outputs zero)\n");
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (dc)                     /* DC start time is based on this */
        ecrt_master_application_time(master,
                (uint64_t) t.tv_sec * 1000000000ULL + (uint64_t) t.tv_nsec);
    if (ecrt_master_activate(master)) {
        fprintf(stderr, "ecrt_master_activate failed\n");
        goto out;
    }
    pd = ecrt_domain_data(domain);
    if (!pd)
        goto out;
    memset(pd, 0, ecrt_domain_size(domain));
    if (mlockall(MCL_CURRENT | MCL_FUTURE))
        perror("mlockall");
    {
        struct sched_param sp = { .sched_priority = 80 };

        if (sched_setscheduler(0, SCHED_FIFO, &sp))
            perror("sched_setscheduler (not real-time)");
    }

    /* With DC, IgH waits up to 5 s per slave for the clock to settle. */
    op_timeout = OP_TIMEOUT_MS;
    for (i = 0; dc && i < n; i++)
        if (devs[i].kind == K_DRIVE)
            op_timeout += 6000;
    clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;) {
        ec_domain_state_t ds;
        int all_op = 1, busy = 0;

        t.tv_nsec += CYCLE_NS;
        if (t.tv_nsec >= 1000000000L) {
            t.tv_nsec -= 1000000000L;
            t.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        {
            /* wake-up latency: DC needs the cycle to be steady */
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            lat = (now.tv_sec - t.tv_sec) * 1000000000L
                + (now.tv_nsec - t.tv_nsec);
            if (lat > lat_max)
                lat_max = lat;
            if (lat > lat_win)
                lat_win = lat;
        }
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        ecrt_domain_state(domain, &ds);

        for (i = 0; i < n; i++) {
            ec_slave_config_state_t st;

            ecrt_slave_config_state(devs[i].sc, &st);
            if (!st.operational || st.al_state != 0x08)
                all_op = 0;
            if (!started && t_wait % 1000 == 999)
                printf("%s S%d:AL 0x%02x", i ? "" : "    waiting", i,
                        st.al_state);
        }
        all_op = all_op && ds.wc_state == EC_WC_COMPLETE;

        /* cable pulled / devices changed: wind down, then exit 3 */
        ecrt_master_state(master, &ms);
        gone = !ms.link_up || ms.slaves_responding != (unsigned int) n
            ? gone + 1 : 0;
        if (gone == 50 && !bus_lost) {
            printf("    bus changed: link %s, %u slave(s) responding\n",
                    ms.link_up ? "up" : "down", ms.slaves_responding);
            bus_lost = 1;
            if (!started)
                break;
        }
        if (!started && t_wait % 1000 == 999) {
            printf("  WC %u  max latency %ld us\n", ds.working_counter,
                    lat_win / 1000);
            lat_win = 0;
        }

        if (!started) {
            if (all_op) {
                started = 1;
                printf("    all slaves OP, start test\n");
                for (i = 0; i < n; i++)
                    for (k = 0; k < devs[i].nax; k++) {
                        int h;

                        for (h = 0; h < SPEED_WIN; h++)
                            devs[i].ax[k].hist[h] = rd(&devs[i],
                                    devs[i].ax[k].apos);
                        set_phase(&devs[i], &devs[i].ax[k], PH_ENABLE, NULL);
                    }
            } else if (++t_wait > op_timeout || stop) {
                printf("    OP not reached (%s)\n", stop ? "stopped"
                        : "timeout; check dmesg");
                break;
            }
        } else {
            run_ms++;
            lost = all_op ? 0 : lost + 1;
            if (lost == 100 && !bus_lost) {
                printf("    bus left OP / working counter incomplete\n");
                bus_lost = 1;
            }
        }
        halt = stop || bus_lost;

        for (i = 0; i < n; i++) {
            if (devs[i].kind == K_DRIVE)
                for (k = 0; k < devs[i].nax; k++) {
                    drive_cycle(&devs[i], &devs[i].ax[k]);
                    busy |= devs[i].ax[k].phase != PH_DONE;
                }
            else if (devs[i].kind == K_IO)
                io_cycle(&devs[i], started);
            busy |= devs[i].kind != K_DRIVE && started && !time_up() && !halt;
        }
        if (started && run_ms % 1000 == 0)
            print_status(devs, n);

        /* everything done: a few more cycles with outputs zero */
        if (started && !busy && t_after < 0) {
            t_after = 0;
            printf("    test finished\n");
        }
        if (t_after >= 0 && ++t_after > 100)
            break;

        if (dc) {
            ecrt_master_application_time(master,
                    (uint64_t) t.tv_sec * 1000000000ULL
                    + (uint64_t) t.tv_nsec);
            ecrt_master_sync_reference_clock(master);
            ecrt_master_sync_slave_clocks(master);
        }
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
    }
    if (started)
        print_status(devs, n);
    printf("    max cycle wake-up latency %ld us%s\n", lat_max / 1000,
            lat_max > 50000 ? " (high: DC may not sync, RT kernel helps)"
            : "");
    rc = bus_lost ? RC_BUS_CHANGED : stop || (started && t_after >= 0) ? 0
        : 1;

out:
    ecrt_master_deactivate(master);
    for (i = 0; i < n; i++) {
        uint8_t buf[2];
        uint32_t abort_code;

        if (!devs[i].has_tq_limit)
            continue;
        EC_WRITE_U16(buf, devs[i].tq_limit_orig);
        if (ecrt_master_sdo_download(master, (uint16_t) i, 0x6072, 0, buf, 2,
                    &abort_code))
            printf("    slave %d: restoring 0x6072 failed (power cycle "
                    "restores it)\n", i);
        else
            printf("    slave %d: Maximum Torque restored to %u\n", i,
                    devs[i].tq_limit_orig);
    }
    ecrt_release_master(master);
    printf("    master released\n");
    return rc;
}
