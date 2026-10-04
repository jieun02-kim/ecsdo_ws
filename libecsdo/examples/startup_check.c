/*
 * startup_check - how control code can use libecsdo_util at startup:
 * read the SDO values it needs by name, then use them directly.
 * Read-only: no master reservation, no OP, no control.
 *
 *   startup_check [CYCLE_US]     (default cycle 1000 us)
 */

#include <stdio.h>
#include <stdlib.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo/ecsdo_util.h"

/* What this control code needs from every drive, by name. */
enum {
    DEVICE_TYPE,
    DEVICE_NAME,
    VENDOR_ID,
    DRIVE_MODES,
    MIN_CYCLE,
    RX_PDO,
    TX_PDO,
    N_WANTED
};

static const char *const wanted[N_WANTED] = {
    [DEVICE_TYPE] = "Device Type",
    [DEVICE_NAME] = "Device Name",
    [VENDOR_ID]   = "Vendor ID",
    [DRIVE_MODES] = "Supported Drive Modes",
    [MIN_CYCLE]   = "Output Sync Manager Parameter/Minimum cycle time",
    [RX_PDO]      = "RxPDO(SM2) Assignment:1",
    [TX_PDO]      = "TxPDO(SM3) Assignment:1",
};

int main(int argc, char **argv)
{
    unsigned long cycle_us = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000;
    ecsdo_item_t *items;
    ecsdo_ctx_t *ctx;
    uint16_t n_slaves, s;
    int k, ret, ok_all = 1;

    if ((ret = ecsdo_open(&ctx, 0))) {
        fprintf(stderr, "open: %s\n", ecsdo_strerror(ret));
        return 1;
    }
    ecsdo_wait_ready(ctx, 30000);
    ecsdo_slave_count(ctx, &n_slaves);

    /* One table: slaves x wanted values. */
    items = calloc((size_t) n_slaves * N_WANTED, sizeof(*items));
    if (!items) {
        ecsdo_close(ctx);
        return 1;
    }
    for (s = 0; s < n_slaves; s++) {
        for (k = 0; k < N_WANTED; k++) {
            items[s * N_WANTED + k].position = s;
            items[s * N_WANTED + k].selector = wanted[k];
        }
    }
    ecsdo_get_items(ctx, items, (size_t) n_slaves * N_WANTED);
    ecsdo_close(ctx); /* all values are in items[] now */

    for (s = 0; s < n_slaves; s++) {
        const ecsdo_item_t *it = &items[s * N_WANTED];
        int ok = 1;

        for (k = 0; k < N_WANTED; k++) {
            if (it[k].result) {
                printf("slave %u: %s: %s\n", s, wanted[k],
                        ecsdo_util_strerror(it[k].result));
                ok = 0;
            }
        }
        if (!ok) {
            ok_all = 0;
            continue;
        }

        /* Values used directly by the control code. */
        uint32_t profile = (uint32_t) it[DEVICE_TYPE].value.u & 0xffff;
        uint32_t modes = (uint32_t) it[DRIVE_MODES].value.u;
        uint32_t min_cycle_ns = (uint32_t) it[MIN_CYCLE].value.u;
        int csp = (modes >> 7) & 1;   /* CiA 402: bit 7 = CSP */

        printf("slave %u: \"%.*s\" vendor 0x%08llx, profile %u, CSP %s, "
                "min cycle %u us, RxPDO 0x%04llx, TxPDO 0x%04llx\n", s,
                (int) it[DEVICE_NAME].value.str_len,
                it[DEVICE_NAME].value.str,
                (unsigned long long) it[VENDOR_ID].value.u, profile,
                csp ? "yes" : "no", min_cycle_ns / 1000,
                (unsigned long long) it[RX_PDO].value.u,
                (unsigned long long) it[TX_PDO].value.u);

        if (profile != 402 || !csp
                || cycle_us * 1000ul < min_cycle_ns) {
            printf("  -> not usable with CSP at %lu us\n", cycle_us);
            ok_all = 0;
        }
    }

    free(items);
    printf("%s\n", ok_all ? "all drives OK for this configuration"
            : "configuration problem");
    return ok_all ? 0 : 1;
}
