/*
 * op_test - bring the bus to OP using libethercat for the basics and
 * libecsdo only for what libethercat cannot do (SDO list, values by name).
 * NO CONTROL: all outputs stay zero (Controlword 0 keeps CiA402 drives in
 * "Switch on disabled"); inputs are only read.
 *
 *   op_test [-c wanted.conf] [--activate]
 *
 * 1. libecsdo     wait until the master has every dictionary.
 * 2. libethercat  reserve the master; slave count, identity and current
 *                 PDO mapping from the master's own scan.
 * 3. libecsdo     wanted names (wanted.conf) -> index:subindex, data type,
 *                 bit length (and current value) via ecsdo_get_items().
 * 4. libethercat  slave configs; register the wanted entries that are in
 *                 the PDO mapping -> process data offsets.
 * 5. --activate   activate, clear outputs, cycle 1 ms until all slaves are
 *                 OP, hold 1 s, print process data decoded with the
 *                 dictionary types, release.
 * Without --activate nothing is written to the slaves.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ecrt.h>

#include <ecsdo/ecsdo.h>
#include <ecsdo/ecsdo_util.h>

#define MAX_SLAVES 16
#define MAX_WANTED 32
#define CYCLE_NS 1000000L
#define OP_TIMEOUT_S 15

typedef struct {
    ec_slave_info_t info;                  /* libethercat: identity */
    ec_slave_config_t *sc;
    size_t n;
    ecsdo_item_t items[MAX_WANTED];        /* libecsdo: names resolved */
    int mapped[MAX_WANTED];                /* in the current PDO mapping */
    int output[MAX_WANTED];                /* 1 = RxPDO (master -> slave) */
    int offset[MAX_WANTED];                /* domain offset, -1 = none */
    unsigned int bit[MAX_WANTED];
} slave_t;

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void) sig;
    stop = 1;
}

/** Wanted names: one per line, '#' comments. */
static int load_wanted(const char *path, char names[][ECSDO_STRING_SIZE * 2])
{
    char line[ECSDO_STRING_SIZE * 2];
    int n = 0;
    FILE *f = fopen(path, "r");

    if (!f) {
        perror(path);
        return -1;
    }
    while (n < MAX_WANTED && fgets(line, sizeof(line), f)) {
        size_t len = strcspn(line, "\r\n");

        line[len] = '\0';
        if (!len || line[0] == '#')
            continue;
        memcpy(names[n++], line, len + 1);
    }
    fclose(f);
    return n;
}

/** Is index:sub in the slave's current PDO mapping (libethercat, from the
 *  master's scan)? Sets *output from the sync manager direction. */
static int find_mapped(ec_master_t *m, const ec_slave_info_t *si,
        uint16_t index, uint8_t sub, int *output)
{
    ec_sync_info_t sync;
    ec_pdo_info_t pdo;
    ec_pdo_entry_info_t entry;
    unsigned int sm, p, e;

    for (sm = 0; sm < si->sync_count; sm++) {
        if (ecrt_master_get_sync_manager(m, si->position, (uint8_t) sm,
                    &sync))
            continue;
        for (p = 0; p < sync.n_pdos; p++) {
            if (ecrt_master_get_pdo(m, si->position, (uint8_t) sm,
                        (uint16_t) p, &pdo))
                continue;
            for (e = 0; e < pdo.n_entries; e++) {
                if (ecrt_master_get_pdo_entry(m, si->position, (uint8_t) sm,
                            (uint16_t) p, (uint16_t) e, &entry))
                    continue;
                if (entry.index == index && entry.subindex == sub) {
                    *output = sync.dir == EC_DIR_OUTPUT;
                    return 1;
                }
            }
        }
    }
    return 0;
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
    static char names[MAX_WANTED][ECSDO_STRING_SIZE * 2];
    static slave_t slaves[MAX_SLAVES];
    const char *conf = "wanted.conf";
    int activate = 0, n_names, a, k, rc = 1, reached = 0;
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
        else if (!strcmp(argv[a], "-c") && a + 1 < argc)
            conf = argv[++a];
        else {
            fprintf(stderr, "usage: %s [-c wanted.conf] [--activate]\n",
                    argv[0]);
            return 2;
        }
    }
    if ((n_names = load_wanted(conf, names)) <= 0)
        return 1;
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
    printf("\n[2] libethercat: slaves and PDO mapping from the master\n");
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

    /* ---- 3. libecsdo: wanted names -> index, type -------------------- */
    printf("\n[3] libecsdo: resolve wanted names (%s)\n", conf);
    for (i = 0; i < n_slaves; i++) {
        slave_t *s = &slaves[i];

        s->n = (size_t) n_names;
        for (k = 0; k < n_names; k++) {
            s->items[k].position = (uint16_t) i;
            s->items[k].selector = names[k];
            s->offset[k] = -1;
        }
        ecsdo_get_items(ctx, s->items, s->n);

        for (k = 0; k < n_names; k++) {
            const ecsdo_item_t *it = &s->items[k];

            printf("    slave %zu %-24s ", i, it->selector);
            if (it->result) {
                printf("ERROR %s\n", ecsdo_util_strerror(it->result));
                continue;
            }
            s->mapped[k] = find_mapped(master, &s->info, it->index,
                    it->subindex, &s->output[k]);
            printf("0x%04X:%02X %-10s %2u bit  %s\n", it->index,
                    it->subindex, ecsdo_type_name(it->data_type),
                    it->bit_length, !s->mapped[k] ? "NOT in PDO mapping"
                    : s->output[k] ? "output PDO" : "input PDO");
        }
    }
    ecsdo_close(ctx); /* nothing more needed from libecsdo */
    ctx = NULL;

    /* ---- 4. libethercat: configuration ------------------------------- */
    printf("\n[4] libethercat: slave configs, PDO entry registration\n");
    domain = ecrt_master_create_domain(master);
    if (!domain)
        goto out;
    for (i = 0; i < n_slaves; i++) {
        slave_t *s = &slaves[i];

        s->sc = ecrt_master_slave_config(master, 0, (uint16_t) i,
                s->info.vendor_id, s->info.product_code);
        if (!s->sc)
            goto out;
        for (k = 0; k < n_names; k++) {
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

    if (!activate) {
        printf("\n[5] not activated (use --activate to go to OP)\n");
        rc = 0;
        goto out;
    }

    /* ---- 5. OP without control ---------------------------------------- */
    printf("\n[5] activate, outputs zero, wait for OP (no control)\n");
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
    while (!stop) {
        ec_slave_config_state_t st;

        t.tv_nsec += CYCLE_NS;
        if (t.tv_nsec >= 1000000000L) {
            t.tv_nsec -= 1000000000L;
            t.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

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
        if ((reached && t.tv_sec - t0.tv_sec >= 1)
                || (!reached && t.tv_sec - t0.tv_sec >= OP_TIMEOUT_S))
            break;

        ecrt_domain_queue(domain);       /* outputs unchanged: zero */
        ecrt_master_send(master);
    }

    if (reached) {
        printf("    process data (inputs read, outputs as sent):\n");
        for (i = 0; i < n_slaves; i++) {
            for (k = 0; k < n_names; k++) {
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
