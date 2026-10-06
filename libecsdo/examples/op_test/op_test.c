/*
 * op_test - bring the bus to OP using libethercat for the basics and
 * libecsdo only for what libethercat cannot do (SDO list, values by name).
 * Without --sine there is NO CONTROL: all outputs stay zero; inputs are
 * only read. --sine LIST moves the listed axes (CiA402 drives, CSP).
 *
 *   op_test [--pdo pdo.conf] [--sdo sdo.conf] [--save FILE | --no-save]
 *           [--activate] [--sine AXIS[,AXIS...]]
 *
 * 1. libecsdo     wait until the master has every dictionary.
 * 2. libethercat  reserve the master; slave count and identity.
 * 3. libecsdo     sdo.conf names -> SDO values read once (drive and motor
 *                 identification; not mapped; default file optional).
 *                 pdo.conf names -> index:subindex, data type,
 *                 bit length, access (and current value) via
 *                 ecsdo_get_items(). Build the PDO mapping from them:
 *                 writable in OP -> RxPDO (output), else TxPDO (input).
 * 4. libethercat  slave configs with that PDO mapping
 *                 (ecrt_slave_config_pdos), register the entries
 *                 -> process data offsets.
 *    JSON         what steps 2-4 read and built is written to op_test.json
 *                 (--save FILE: elsewhere, --no-save: not at all), before
 *                 activating, so also without --activate.
 * 5. --activate   activate, clear outputs, cycle 1 ms until all slaves are
 *                 OP, hold 1 s, print process data decoded with the
 *                 dictionary types and the working counter, release.
 *    --sine LIST  (implies --activate) instead of holding: enable the
 *                 listed axes (CiA402 state machine), take each actual
 *                 position as its start position, run a slow sine around
 *                 it (amplitude ramped in and out), disable, release.
 *                 Needs Controlword, Statusword, Target Position and
 *                 Position Actual Value in pdo.conf; counts/rev comes
 *                 from "Encoder Resolution" (SDO); mode CSP is set with a
 *                 startup SDO (0x6060 = 8). Fault, following error, lost
 *                 working counter or Ctrl+C -> quick stop.
 * Without --activate nothing is written to the slaves. With it, the master
 * writes the new mapping (0x1C12/0x1C13, 0x1600/0x1A00) in PREOP.
 */

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ecrt.h>

#include <ecsdo/ecsdo.h>
#include <ecsdo/ecsdo_util.h>

#define MAX_SLAVES 16
#define MAX_PDO 32
#define MAX_SDO 48
#define CYCLE_NS 1000000L
#define OP_TIMEOUT_S 15

/* Where the pdo.conf entries are mapped (CiA402 / ETG standard, L7N: SM2 =
 * 0x1C12 RxPDO assignment, SM3 = 0x1C13 TxPDO assignment). */
#define SM_OUT 2
#define SM_IN 3
#define RXPDO 0x1600
#define TXPDO 0x1A00

/* CiA402 objects, controlword commands and statusword states. */
#define OBJ_CONTROLWORD 0x6040
#define OBJ_STATUSWORD 0x6041
#define OBJ_MODES_OF_OPERATION 0x6060
#define OBJ_POSITION_ACTUAL 0x6064
#define OBJ_TARGET_POSITION 0x607A
#define MODE_CSP 8
#define CW_DISABLE_VOLTAGE 0x0000
#define CW_QUICK_STOP 0x0002
#define CW_SHUTDOWN 0x0006
#define CW_SWITCH_ON 0x0007
#define CW_ENABLE_OPERATION 0x000F
#define SW_FAULT 0x0008
#define SW_STATE_MASK 0x006F
#define SW_SWITCH_ON_DISABLED 0x0040          /* mask 0x004F */
#define SW_READY_TO_SWITCH_ON 0x0021
#define SW_SWITCHED_ON 0x0023
#define SW_OPERATION_ENABLED 0x0027

/* Sine test: slow and small. 0.25 rev, 10 s -> peak 9.4 rpm. */
#define SINE_AMPL_REV 0.25
#define SINE_PERIOD_S 10.0
#define SINE_PERIODS 3          /* ramp in, full amplitude, ramp out */
#define MAX_FOLLOW_REV 0.1      /* software following error limit */
#define ENABLE_TIMEOUT_S 5
#define STOP_CYCLES 500         /* keep sending the stop command 0.5 s */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    ec_slave_info_t info;                  /* libethercat: identity */
    ec_slave_config_t *sc;
    size_t n;
    ecsdo_item_t items[MAX_PDO];           /* libecsdo: names resolved */
    int mapped[MAX_PDO];                   /* put in our PDO mapping */
    int output[MAX_PDO];                   /* 1 = RxPDO (master -> slave) */
    ec_pdo_entry_info_t rx[MAX_PDO];       /* mapping built in step 3 */
    ec_pdo_entry_info_t tx[MAX_PDO];
    unsigned int n_rx, n_tx;
    int offset[MAX_PDO];                   /* domain offset, -1 = none */
    unsigned int bit[MAX_PDO];

    ecsdo_item_t sdo_values[MAX_SDO];      /* sdo.conf values */

    /* --sine */
    int axis;                              /* selected with --sine */
    ecsdo_item_t enc;                      /* "Encoder Resolution" (SDO) */
    long counts_rev;
    int o_cw, o_sw, o_tp, o_pa;            /* offsets of the CiA402 entries */
    long ampl, max_follow;                 /* in counts */
    int enabled;
    uint16_t sw;
    int32_t pa, p0, tp;                    /* actual, start, target */
} slave_t;

enum { SINE_ENABLING, SINE_RUNNING, SINE_STOPPING };

typedef struct {
    int phase;
    long cycle;                            /* cycles in this phase */
    uint16_t stop_cw;
} sine_t;

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void) sig;
    stop = 1;
}

/** Names (pdo.conf, sdo.conf): one per line, '#' comments. */
static int load_names(const char *path, char names[][ECSDO_STRING_SIZE * 2],
        int max)
{
    char line[ECSDO_STRING_SIZE * 2];
    int n = 0;
    FILE *f = fopen(path, "r");

    if (!f) {
        perror(path);
        return -1;
    }
    while (n < max && fgets(line, sizeof(line), f)) {
        size_t len = strcspn(line, "\r\n");

        line[len] = '\0';
        if (!len || line[0] == '#')
            continue;
        memcpy(names[n++], line, len + 1);
    }
    fclose(f);
    return n;
}

static void print_pd(const ecsdo_item_t *it, const uint8_t *data)
{
    ecsdo_value_t v;
    size_t bytes = (it->bit_length + 7u) / 8u;

    if (ecsdo_decode(it->data_type, data, bytes, &v) != ECSDO_OK)
        printf("(raw %zu byte)", bytes);
    else if (v.kind == ECSDO_VALUE_UNSIGNED)
        printf("0x%0*llx", (int) (2 * bytes), (unsigned long long) v.u);
    else if (v.kind == ECSDO_VALUE_SIGNED)
        printf("%lld", (long long) v.i);
    else if (v.kind == ECSDO_VALUE_REAL)
        printf("%g", v.f);
    else
        printf("(%s)", ecsdo_type_name(it->data_type));
}

/** --sine list "0,1,3" -> slaves[].axis. Returns the count or -1. */
static int parse_axes(const char *list, slave_t *s, size_t n)
{
    const char *p = list;
    char *end;
    int count = 0;

    while (*p) {
        unsigned long v = strtoul(p, &end, 10);

        if (end == p || v >= n)
            return -1;
        s[v].axis = 1;
        count++;
        if (*end == ',')
            end++;
        else if (*end)
            return -1;
        p = end;
    }
    return count;
}

/** Domain offset of a registered entry, -1 if not registered. */
static int offset_of(const slave_t *s, uint16_t index)
{
    size_t k;

    for (k = 0; k < s->n; k++) {
        if (!s->items[k].result && s->offset[k] >= 0
                && s->items[k].index == index)
            return s->offset[k];
    }
    return -1;
}

static void sine_stop(sine_t *g, uint16_t cw, const char *why)
{
    printf("    sine: %s -> controlword 0x%04x\n", why, cw);
    g->phase = SINE_STOPPING;
    g->cycle = 0;
    g->stop_cw = cw;
}

/** One 1 ms cycle of the sine test: read inputs and check them, advance
 *  the phase, write Controlword and Target Position of the selected axes.
 *  Returns 1 when finished (after the stop command was sent long enough). */
static int sine_cycle(sine_t *g, slave_t *sl, size_t n, uint8_t *pd,
        int wc_ok)
{
    double t = (double) g->cycle * CYCLE_NS / 1e9;
    double total = SINE_PERIODS * SINE_PERIOD_S;
    int all_enabled = 1;
    size_t i;

    /* Inputs and checks. */
    for (i = 0; i < n; i++) {
        slave_t *s = &sl[i];
        uint16_t state;

        if (!s->axis)
            continue;
        s->sw = EC_READ_U16(pd + s->o_sw);
        s->pa = EC_READ_S32(pd + s->o_pa);
        state = s->sw & SW_STATE_MASK;
        if (g->phase == SINE_STOPPING)
            continue;
        if (s->sw & SW_FAULT) {
            printf("    axis %zu: fault, statusword 0x%04x\n", i, s->sw);
            sine_stop(g, CW_QUICK_STOP, "fault");
        } else if (g->phase == SINE_RUNNING
                && state != SW_OPERATION_ENABLED) {
            printf("    axis %zu: left Operation enabled, statusword "
                    "0x%04x\n", i, s->sw);
            sine_stop(g, CW_QUICK_STOP, "axis disabled");
        } else if (g->phase == SINE_RUNNING
                && labs((long) s->tp - s->pa) > s->max_follow) {
            printf("    axis %zu: following error %ld counts (target %d, "
                    "actual %d)\n", i, (long) s->tp - s->pa, s->tp, s->pa);
            sine_stop(g, CW_QUICK_STOP, "following error");
        } else if (g->phase == SINE_ENABLING && !s->enabled
                && state == SW_OPERATION_ENABLED) {
            s->enabled = 1;
            s->p0 = s->tp = s->pa;     /* start position = actual */
            printf("    axis %zu: Operation enabled, start position %d\n",
                    i, s->p0);
        }
        if (!s->enabled)
            all_enabled = 0;
    }

    /* Phase. */
    if (g->phase != SINE_STOPPING) {
        if (stop)
            sine_stop(g, CW_QUICK_STOP, "stop requested");
        else if (!wc_ok)
            sine_stop(g, CW_QUICK_STOP, "working counter incomplete");
    }
    if (g->phase == SINE_ENABLING) {
        if (all_enabled) {
            printf("    sine: all axes enabled, running %.0f s\n", total);
            g->phase = SINE_RUNNING;
            g->cycle = 0;
            t = 0.0;
        } else if (g->cycle >= ENABLE_TIMEOUT_S * 1000L) {
            sine_stop(g, CW_SHUTDOWN, "not all axes enabled");
        }
    } else if (g->phase == SINE_RUNNING && t >= total) {
        sine_stop(g, CW_SHUTDOWN, "done");
    } else if (g->phase == SINE_STOPPING && g->cycle >= STOP_CYCLES) {
        return 1;
    }

    /* Outputs. */
    for (i = 0; i < n; i++) {
        slave_t *s = &sl[i];
        uint16_t state = s->sw & SW_STATE_MASK, cw;
        double env;

        if (!s->axis)
            continue;
        if (g->phase == SINE_STOPPING) {
            cw = g->stop_cw;           /* target held */
        } else if (g->phase == SINE_RUNNING) {
            env = t < SINE_PERIOD_S ? t / SINE_PERIOD_S
                : t > total - SINE_PERIOD_S ? (total - t) / SINE_PERIOD_S
                : 1.0;
            s->tp = s->p0 + (int32_t) lround(env * (double) s->ampl
                    * sin(2.0 * M_PI * t / SINE_PERIOD_S));
            cw = CW_ENABLE_OPERATION;
        } else {
            if (!s->enabled)
                s->tp = s->pa;         /* no jump when the drive enables */
            if ((s->sw & 0x004F) == SW_SWITCH_ON_DISABLED)
                cw = CW_SHUTDOWN;
            else if (state == SW_READY_TO_SWITCH_ON)
                cw = CW_SWITCH_ON;
            else if (state == SW_SWITCHED_ON
                    || state == SW_OPERATION_ENABLED)
                cw = CW_ENABLE_OPERATION;
            else
                cw = CW_DISABLE_VOLTAGE;
        }
        EC_WRITE_U16(pd + s->o_cw, cw);
        EC_WRITE_S32(pd + s->o_tp, s->tp);
    }
    g->cycle++;
    return 0;
}

static void json_str(FILE *f, const char *str)
{
    fputc('"', f);
    for (; *str; str++) {
        if (*str == '"' || *str == '\\')
            fprintf(f, "\\%c", *str);
        else if ((unsigned char) *str < 0x20)
            fprintf(f, "\\u%04x", (unsigned char) *str);
        else
            fputc(*str, f);
    }
    fputc('"', f);
}

static void json_entries(FILE *f, const ec_pdo_entry_info_t *e,
        unsigned int n)
{
    unsigned int j;

    fputc('[', f);
    for (j = 0; j < n; j++)
        fprintf(f, "%s{\"index\": \"0x%04x\", \"subindex\": %u, "
                "\"bit_length\": %u}", j ? ", " : "", e[j].index,
                e[j].subindex, e[j].bit_length);
    fputc(']', f);
}

/** --save: identity (libethercat), resolved items (libecsdo
 *  "ecsdo-items"), the PDO mapping built from them, registered offsets
 *  and the --sine setup, per slave. */
static int save_json(const char *path, const char *pdo_conf,
        const char *sdo_conf, const slave_t *sl, size_t n, size_t n_sdo)
{
    FILE *f = fopen(path, "w");
    size_t i, k;
    int bad;

    if (!f) {
        perror(path);
        return -1;
    }
    fputs("{\"format\": \"op_test\", \"version\": 1, \"master\": 0, "
            "\"pdo_conf\": ", f);
    json_str(f, pdo_conf);
    fputs(", \"sdo_conf\": ", f);
    if (n_sdo)
        json_str(f, sdo_conf);
    else
        fputs("null", f);
    fputs(", \"slaves\": [", f);
    for (i = 0; i < n; i++) {
        const slave_t *s = &sl[i];
        int first = 1;

        fprintf(f, "%s\n{\"position\": %zu, \"name\": ", i ? "," : "", i);
        json_str(f, s->info.name);
        fprintf(f, ", \"vendor_id\": \"0x%08x\", \"product_code\": "
                "\"0x%08x\", \"revision\": \"0x%08x\", \"serial\": "
                "\"0x%08x\",\n\"pdo_items\": ", s->info.vendor_id,
                s->info.product_code, s->info.revision_number,
                s->info.serial_number);
        ecsdo_items_json(s->items, s->n, f);
        fputs(",\"sdo_values\": ", f);
        if (n_sdo)
            ecsdo_items_json(s->sdo_values, n_sdo, f);
        else
            fputs("null", f);
        fprintf(f, ",\"pdo_mapping\": {\"output\": {\"sm\": %d, "
                "\"pdo\": \"0x%04x\", \"entries\": ", SM_OUT, RXPDO);
        json_entries(f, s->rx, s->n_rx);
        fprintf(f, "}, \"input\": {\"sm\": %d, \"pdo\": \"0x%04x\", "
                "\"entries\": ", SM_IN, TXPDO);
        json_entries(f, s->tx, s->n_tx);
        fputs("}},\n\"registered\": [", f);
        for (k = 0; k < s->n; k++) {
            if (s->offset[k] < 0)
                continue;
            fprintf(f, "%s{\"selector\": ", first ? "" : ", ");
            json_str(f, s->items[k].selector);
            fprintf(f, ", \"index\": \"0x%04x\", \"subindex\": %u, "
                    "\"direction\": \"%s\", \"offset\": %d, \"bit\": %u}",
                    s->items[k].index, s->items[k].subindex,
                    s->output[k] ? "output" : "input", s->offset[k],
                    s->bit[k]);
            first = 0;
        }
        fputs("],\n\"sine\": ", f);
        if (s->axis) {
            fputs("{\"encoder_resolution\": ", f);
            ecsdo_items_json(&s->enc, 1, f);
            fprintf(f, ", \"counts_per_rev\": %ld, \"amplitude\": %ld, "
                    "\"period_s\": %g, \"periods\": %d, "
                    "\"max_following_error\": %ld}", s->counts_rev,
                    s->ampl, SINE_PERIOD_S, SINE_PERIODS, s->max_follow);
        } else {
            fputs("null", f);
        }
        fputc('}', f);
    }
    fputs("\n]}\n", f);
    bad = ferror(f);
    if (fclose(f) || bad) {
        fprintf(stderr, "%s: write error\n", path);
        return -1;
    }
    printf("    saved %s\n", path);
    return 0;
}

static int all_op(slave_t *s, size_t n)
{
    ec_slave_config_state_t st;
    size_t i;

    for (i = 0; i < n; i++) {
        ecrt_slave_config_state(s[i].sc, &st);
        if (!st.operational || st.al_state != ECSDO_AL_OP)
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    static char pdo_names[MAX_PDO][ECSDO_STRING_SIZE * 2];
    static char sdo_names[MAX_SDO][ECSDO_STRING_SIZE * 2];
    static slave_t slaves[MAX_SLAVES];
    const char *pdo_conf = "pdo.conf", *sine_list = NULL;
    const char *save = "op_test.json";
    const char *sdo_conf = "sdo.conf";
    int sdo_given = 0, n_sdo = 0;
    int activate = 0, n_pdo, a, k, rc = 1, reached = 0, done = 0;
    long wc_bad = 0, wc_cycles = 0;
    sine_t sine = { SINE_ENABLING, 0, 0 };
    size_t n_slaves, i, total = 0;
    ec_master_t *master = NULL;
    ec_domain_t *domain;
    ec_master_info_t mi;
    unsigned int last[MAX_SLAVES];
    struct timespec t, t0;
    ecsdo_ctx_t *ctx;
    uint8_t *pd;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--activate"))
            activate = 1;
        else if (!strcmp(argv[a], "--pdo") && a + 1 < argc)
            pdo_conf = argv[++a];
        else if (!strcmp(argv[a], "--sdo") && a + 1 < argc) {
            sdo_conf = argv[++a];
            sdo_given = 1;
        } else if (!strcmp(argv[a], "--save") && a + 1 < argc)
            save = argv[++a];
        else if (!strcmp(argv[a], "--no-save"))
            save = NULL;
        else if (!strcmp(argv[a], "--sine") && a + 1 < argc) {
            sine_list = argv[++a];
            activate = 1;
        } else {
            fprintf(stderr, "usage: %s [--pdo pdo.conf] [--sdo sdo.conf] "
                    "[--save FILE | --no-save] [--activate] "
                    "[--sine AXIS[,AXIS...]]\n",
                    argv[0]);
            return 2;
        }
    }
    if ((n_pdo = load_names(pdo_conf, pdo_names, MAX_PDO)) <= 0)
        return 1;
    /* The default sdo.conf is optional, an explicit --sdo file is not. */
    if (sdo_given || access(sdo_conf, R_OK) == 0) {
        if ((n_sdo = load_names(sdo_conf, sdo_names, MAX_SDO)) < 0)
            return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ---- 1. libecsdo: dictionaries complete? ------------------------- */
    if (ecsdo_open(&ctx, 0) != ECSDO_OK) {
        fprintf(stderr, "ecsdo_open failed\n");
        return 1;
    }
    printf("[1] libecsdo: wait for dictionaries\n");
    if (ecsdo_wait_ready(ctx, 30000) == ECSDO_ERR_TIMEOUT)
        printf("    warning: some dictionaries not complete\n");

    /* ---- 2. libethercat: master's scan ------------------------------- */
    printf("\n[2] libethercat: slaves from the master\n");
    master = ecrt_request_master(0);
    if (!master) {
        fprintf(stderr, "ecrt_request_master failed (master in use?)\n");
        ecsdo_close(ctx);
        return 1;
    }
    if (ecrt_master(master, &mi))
        goto out;
    n_slaves = mi.slave_count < MAX_SLAVES ? mi.slave_count : MAX_SLAVES;
    for (i = 0; i < n_slaves; i++) {
        if (ecrt_master_get_slave(master, (uint16_t) i, &slaves[i].info))
            goto out;
        printf("    slave %zu \"%s\" 0x%08x:0x%08x\n", i,
                slaves[i].info.name, slaves[i].info.vendor_id,
                slaves[i].info.product_code);
    }
    if (sine_list && parse_axes(sine_list, slaves, n_slaves) <= 0) {
        fprintf(stderr, "--sine %s: need slave positions 0..%zu\n",
                sine_list, n_slaves - 1);
        goto out;
    }

    /* ---- 3. libecsdo: SDO values; pdo.conf -> PDO mapping ------------ */
    printf("\n[3] libecsdo: read SDO values, resolve %s, build PDO mapping\n",
            pdo_conf);
    for (i = 0; i < n_slaves; i++) {
        slave_t *s = &slaves[i];

        s->n = (size_t) n_pdo;
        for (k = 0; k < n_pdo; k++) {
            s->items[k].position = (uint16_t) i;
            s->items[k].selector = pdo_names[k];
            s->offset[k] = -1;
        }
        if (n_sdo) {
            int failed;

            for (k = 0; k < n_sdo; k++) {
                s->sdo_values[k].position = (uint16_t) i;
                s->sdo_values[k].selector = sdo_names[k];
            }
            failed = ecsdo_get_items(ctx, s->sdo_values, (size_t) n_sdo);
            printf("    slave %zu %s: %d values read, %d failed\n", i,
                    sdo_conf, n_sdo - (failed > 0 ? failed : 0),
                    failed);
            for (k = 0; k < n_sdo; k++) {
                if (s->sdo_values[k].result)
                    printf("    slave %zu sdo %-24s ERROR %s\n", i,
                            s->sdo_values[k].selector,
                            ecsdo_util_strerror(s->sdo_values[k].result));
            }
        }
        ecsdo_get_items(ctx, s->items, s->n);

        for (k = 0; k < n_pdo; k++) {
            const ecsdo_item_t *it = &s->items[k];
            ec_pdo_entry_info_t *e;

            printf("    slave %zu %-24s ", i, it->selector);
            if (it->result) {
                printf("ERROR %s\n", ecsdo_util_strerror(it->result));
                continue;
            }
            printf("0x%04X:%02X %-10s %2u bit  ", it->index, it->subindex,
                    ecsdo_type_name(it->data_type), it->bit_length);
            if (!it->bit_length || it->bit_length > 64) {
                printf("NOT mappable (size)\n");
                continue;
            }
            /* The master writes outputs in OP, so an entry the slave
             * accepts writes to in OP goes to the RxPDO. */
            s->output[k] = it->write_access[ECSDO_ACCESS_OP] != 0;
            e = s->output[k] ? &s->rx[s->n_rx++] : &s->tx[s->n_tx++];
            e->index = it->index;
            e->subindex = it->subindex;
            e->bit_length = (uint8_t) it->bit_length;
            s->mapped[k] = 1;
            printf("-> %s 0x%04X\n", s->output[k] ? "RxPDO" : "TxPDO",
                    s->output[k] ? RXPDO : TXPDO);
        }

        if (!s->axis)
            continue;
        /* Position unit = encoder count (Electric Gear Mode 0). */
        s->enc.position = (uint16_t) i;
        s->enc.selector = "Encoder Resolution";
        if (ecsdo_get_items(ctx, &s->enc, 1)
                || s->enc.value.kind != ECSDO_VALUE_UNSIGNED
                || s->enc.value.u < 10 || s->enc.value.u > 24) {
            fprintf(stderr, "slave %zu: no usable Encoder Resolution\n", i);
            goto out;
        }
        s->counts_rev = 1L << s->enc.value.u;
        printf("    slave %zu Encoder Resolution %llu bit -> %ld counts/rev\n",
                i, (unsigned long long) s->enc.value.u, s->counts_rev);
    }
    ecsdo_close(ctx); /* nothing more needed from libecsdo */
    ctx = NULL;

    /* ---- 4. libethercat: configuration ------------------------------- */
    printf("\n[4] libethercat: slave configs with the PDO mapping, "
            "entry registration\n");
    domain = ecrt_master_create_domain(master);
    if (!domain)
        goto out;
    for (i = 0; i < n_slaves; i++) {
        slave_t *s = &slaves[i];
        ec_pdo_info_t pdos[2] = {
            { RXPDO, s->n_rx, s->rx },
            { TXPDO, s->n_tx, s->tx },
        };
        ec_sync_info_t syncs[] = {
            { SM_OUT, EC_DIR_OUTPUT, 1, &pdos[0], EC_WD_DEFAULT },
            { SM_IN, EC_DIR_INPUT, 1, &pdos[1], EC_WD_DEFAULT },
            { 0xff, 0, 0, NULL, 0 }
        };

        s->sc = ecrt_master_slave_config(master, 0, (uint16_t) i,
                s->info.vendor_id, s->info.product_code);
        if (!s->sc)
            goto out;
        /* Only stored in the master here; written to the slave on
         * activate. Replaces the slave's current mapping on SM2/SM3. */
        if (ecrt_slave_config_pdos(s->sc, EC_END, syncs)) {
            fprintf(stderr, "slave %zu: ecrt_slave_config_pdos failed\n", i);
            goto out;
        }
        printf("    slave %zu: 0x%04X %u entries, 0x%04X %u entries\n", i,
                RXPDO, s->n_rx, TXPDO, s->n_tx);
        for (k = 0; k < n_pdo; k++) {
            if (s->items[k].result || !s->mapped[k])
                continue;
            s->offset[k] = ecrt_slave_config_reg_pdo_entry(s->sc,
                    s->items[k].index, s->items[k].subindex, domain,
                    &s->bit[k]);
            if (s->offset[k] < 0) {
                printf("    slave %zu %-24s -> FAILED\n", i,
                        s->items[k].selector);
                continue;
            }
            printf("    slave %zu %-24s -> offset %2d bit %u\n", i,
                    s->items[k].selector, s->offset[k], s->bit[k]);
            total++;
        }
    }
    printf("    %zu entries registered\n", total);

    for (i = 0; i < n_slaves; i++) {
        slave_t *s = &slaves[i];

        if (!s->axis)
            continue;
        s->o_cw = offset_of(s, OBJ_CONTROLWORD);
        s->o_sw = offset_of(s, OBJ_STATUSWORD);
        s->o_tp = offset_of(s, OBJ_TARGET_POSITION);
        s->o_pa = offset_of(s, OBJ_POSITION_ACTUAL);
        if (s->o_cw < 0 || s->o_sw < 0 || s->o_tp < 0 || s->o_pa < 0) {
            fprintf(stderr, "slave %zu: --sine needs Controlword, Statusword, "
                    "Target Position, Position Actual Value\n", i);
            goto out;
        }
        /* Written by the master in PREOP, before SAFEOP. */
        if (ecrt_slave_config_sdo8(s->sc, OBJ_MODES_OF_OPERATION, 0,
                    MODE_CSP))
            goto out;
        s->ampl = lround(SINE_AMPL_REV * (double) s->counts_rev);
        s->max_follow = lround(MAX_FOLLOW_REV * (double) s->counts_rev);
        printf("    axis %zu: CSP, sine %ld counts (%.2f rev), period %.0f s, "
                "peak %.1f rpm, following error limit %ld counts\n", i,
                s->ampl, SINE_AMPL_REV, SINE_PERIOD_S,
                SINE_AMPL_REV * 2.0 * M_PI / SINE_PERIOD_S * 60.0,
                s->max_follow);
    }

    if (save && save_json(save, pdo_conf, sdo_conf, slaves, n_slaves,
                (size_t) n_sdo))
        goto out;

    if (!activate) {
        printf("\n[5] not activated (use --activate to go to OP)\n");
        rc = 0;
        goto out;
    }

    /* ---- 5. OP without control ---------------------------------------- */
    printf(sine_list ? "\n[5] activate, outputs zero, wait for OP, sine\n"
            : "\n[5] activate, outputs zero, wait for OP (no control)\n");
    if (ecrt_master_activate(master)) {
        fprintf(stderr, "ecrt_master_activate failed\n");
        goto out;
    }
    pd = ecrt_domain_data(domain);
    if (!pd)
        goto out;
    /* Userspace process data is not zero-initialised: clear it before the
     * first frame. Outputs are never written afterwards. */
    memset(pd, 0, ecrt_domain_size(domain));

    for (i = 0; i < n_slaves; i++)
        last[i] = 0xff;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t0 = t;
    for (;;) {
        ec_slave_config_state_t st;
        ec_domain_state_t ds;

        t.tv_nsec += CYCLE_NS;
        if (t.tv_nsec >= 1000000000L) {
            t.tv_nsec -= 1000000000L;
            t.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        ecrt_domain_state(domain, &ds);

        for (i = 0; i < n_slaves; i++) {
            ecrt_slave_config_state(slaves[i].sc, &st);
            if (st.al_state != last[i]) {
                printf("    slave %zu: AL 0x%02x%s\n", i, st.al_state,
                        st.operational ? " operational" : "");
                last[i] = st.al_state;
            }
        }
        if (!reached && all_op(slaves, n_slaves)) {
            reached = 1;
            t0 = t;
            printf("    all slaves OP\n");
        }
        if (reached) {
            wc_cycles++;
            if (ds.wc_state != EC_WC_COMPLETE)
                wc_bad++;
        }

        if (!reached)
            done = stop || t.tv_sec - t0.tv_sec >= OP_TIMEOUT_S;
        else if (!sine_list)
            done = stop || t.tv_sec - t0.tv_sec >= 1;
        else
            done = sine_cycle(&sine, slaves, n_slaves, pd,
                    ds.wc_state == EC_WC_COMPLETE);
        if (done)
            break;

        ecrt_domain_queue(domain);       /* outputs: zero unless --sine */
        ecrt_master_send(master);
    }

    if (reached) {
        printf("    working counter incomplete in %ld of %ld cycles\n",
                wc_bad, wc_cycles);
        printf("    process data (inputs read, outputs as sent):\n");
        for (i = 0; i < n_slaves; i++) {
            for (k = 0; k < n_pdo; k++) {
                if (slaves[i].offset[k] < 0)
                    continue;
                printf("      slave %zu %-6s %-24s = ", i,
                        slaves[i].output[k] ? "out" : "in",
                        slaves[i].items[k].selector);
                print_pd(&slaves[i].items[k], pd + slaves[i].offset[k]);
                printf("\n");
            }
        }
        rc = 0;
    } else {
        printf("    OP not reached within %d s\n", OP_TIMEOUT_S);
    }

out:
    if (ctx)
        ecsdo_close(ctx);
    ecrt_release_master(master);
    printf("\n    master released\n");
    return rc;
}
