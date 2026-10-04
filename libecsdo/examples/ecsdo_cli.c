/*
 * ecsdo_cli - minimal libecsdo v2 example and check tool.
 *
 *   ecsdo_cli [-m MASTER] -p POS sdos            like `ethercat sdos -p POS`
 *   ecsdo_cli [-m MASTER] scan                   dictionaries as JSON
 *   ecsdo_cli [-m MASTER] [-p POS] get [-j] SELECTOR...
 *                                          values by name (all slaves
 *                                          if no -p), text or JSON
 *   ecsdo_cli [-m MASTER] -p POS upload [-t raw] IDX SUB
 *                                          like `ethercat upload -p POS
 *                                          [-t raw] IDX SUB`
 *
 * Output is meant to be diffed against the ethercat tool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo/ecsdo_util.h"

/* Type names as printed by `ethercat sdos` (that tool's output strings,
 * for diffing); other types print as "type XXXX". */
static const char *tool_type_name(uint16_t t)
{
    static const struct {
        uint16_t code;
        const char *name;
    } names[] = {
        {0x0001, "bool"}, {0x0002, "int8"}, {0x0003, "int16"},
        {0x0004, "int32"}, {0x0005, "uint8"}, {0x0006, "uint16"},
        {0x0007, "uint32"}, {0x0008, "float"}, {0x0009, "string"},
        {0x000a, "octet_string"}, {0x000b, "unicode_string"},
        {0x0010, "int24"}, {0x0011, "double"}, {0x0012, "int40"},
        {0x0013, "int48"}, {0x0014, "int56"}, {0x0015, "int64"},
        {0x0016, "uint24"}, {0x0018, "uint40"}, {0x0019, "uint48"},
        {0x001a, "uint56"}, {0x001b, "uint64"}, {0xfffb, "sm8"},
        {0xfffc, "sm16"}, {0xfffd, "sm32"}, {0xfffe, "sm64"},
        {0xffff, "raw"},
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (names[i].code == t)
            return names[i].name;
    }
    return NULL;
}

static int cmd_sdos(ecsdo_ctx_t *ctx, uint16_t pos)
{
    ecsdo_slave_t *s;
    size_t i, j;
    int ret;

    /* Dictionaries are fetched by the master in the background. */
    if (ecsdo_wait_ready(ctx, 30000) == ECSDO_ERR_TIMEOUT)
        fprintf(stderr, "warning: dictionaries not complete\n");

    ret = ecsdo_read_dict(ctx, pos, &s);
    if (ret) {
        fprintf(stderr, "read_dict: %s\n", ecsdo_strerror(ret));
        return 1;
    }
    if (s->status != ECSDO_SLAVE_OK)
        fprintf(stderr, "slave %u: status %d\n", pos, (int) s->status);

    for (i = 0; i < s->object_count; i++) {
        const ecsdo_object_t *o = &s->objects[i];

        printf("SDO 0x%04x, \"%s\"\n", o->index, o->name);
        for (j = 0; j < o->entry_count; j++) {
            const ecsdo_entry_t *e = &o->entries[j];
            const char *tn = tool_type_name(e->data_type);

            printf("  0x%04x:%02x, %c%c%c%c%c%c, ", o->index, e->subindex,
                    e->read_access[ECSDO_ACCESS_PREOP] ? 'r' : '-',
                    e->write_access[ECSDO_ACCESS_PREOP] ? 'w' : '-',
                    e->read_access[ECSDO_ACCESS_SAFEOP] ? 'r' : '-',
                    e->write_access[ECSDO_ACCESS_SAFEOP] ? 'w' : '-',
                    e->read_access[ECSDO_ACCESS_OP] ? 'r' : '-',
                    e->write_access[ECSDO_ACCESS_OP] ? 'w' : '-');
            if (tn)
                printf("%s", tn);
            else
                printf("type %04x", e->data_type);
            printf(", %u bit, \"%s\"\n", e->bit_length, e->description);
        }
    }
    ecsdo_free_slave(s);
    return 0;
}

static int cmd_upload(ecsdo_ctx_t *ctx, uint16_t pos, uint16_t index,
        uint8_t sub, int raw)
{
    static uint8_t buf[64 * 1024];
    ecsdo_value_t v;
    uint32_t abort_code;
    size_t i, n;
    int ret;

    if (raw) {
        ret = ecsdo_upload(ctx, pos, index, sub, buf, sizeof(buf), &n,
                &abort_code);
        v.data = buf;
        v.size = n;
        v.kind = ECSDO_VALUE_RAW;
    } else {
        /* Type check + upload + decode, like `ethercat upload`. */
        ret = ecsdo_upload_value(ctx, pos, index, sub, buf, sizeof(buf), &v,
                &abort_code);
    }

    if (ret == ECSDO_ERR_ABORT) {
        const char *t = ecsdo_abort_string(abort_code);
        fprintf(stderr, "SDO abort 0x%08x: %s\n", abort_code,
                t ? t : "unknown");
        return 1;
    }
    if (ret && ret != ECSDO_ERR_UNKNOWN_TYPE
            && ret != ECSDO_ERR_TYPE_MISMATCH) {
        fprintf(stderr, "upload: %s\n", ecsdo_strerror(ret));
        return 1;
    }
    if (ret) /* transferred, but not decodable: show raw bytes */
        fprintf(stderr, "%s (type 0x%04x, %zu byte)\n", ecsdo_strerror(ret),
                v.data_type, v.size);

    switch (v.kind) {
        case ECSDO_VALUE_UNSIGNED:
            printf("0x%0*llx %llu\n", (int) (2 * v.size),
                    (unsigned long long) v.u, (unsigned long long) v.u);
            break;
        case ECSDO_VALUE_SIGNED: {
            /* hex of the raw bits, decimal with sign */
            unsigned long long bits = (unsigned long long) v.i;
            if (v.size < 8)
                bits &= (1ULL << (8 * v.size)) - 1;
            printf("0x%0*llx %lld\n", (int) (2 * v.size), bits,
                    (long long) v.i);
            break;
        }
        case ECSDO_VALUE_REAL:
            printf("%g\n", v.f);
            break;
        case ECSDO_VALUE_STRING:
            printf("%.*s\n", (int) v.str_len, v.str);
            break;
        default:
            for (i = 0; i < v.size; i++)
                printf("0x%02x%s", v.data[i], i + 1 < v.size ? " " : "");
            printf("\n");
            break;
    }
    return ret ? 1 : 0;
}

/* scan: dictionaries of all slaves as JSON */
static int cmd_scan(ecsdo_ctx_t *ctx, unsigned int master)
{
    int ret;

    if (ecsdo_wait_ready(ctx, 30000) == ECSDO_ERR_TIMEOUT)
        fprintf(stderr, "warning: dictionaries not complete\n");
    ret = ecsdo_dump_dict_json(ctx, master, stdout);
    if (ret)
        fprintf(stderr, "scan: %s\n", ecsdo_util_strerror(ret));
    return ret ? 1 : 0;
}

/* get: values by name for one slave or all */
static int cmd_get(ecsdo_ctx_t *ctx, unsigned long pos, int json,
        char **sel, int n_sel)
{
    ecsdo_item_t *items;
    uint16_t count, first, last, p;
    size_t n = 0, i;
    int ret, k;

    if (ecsdo_wait_ready(ctx, 30000) == ECSDO_ERR_TIMEOUT)
        fprintf(stderr, "warning: dictionaries not complete\n");
    if ((ret = ecsdo_slave_count(ctx, &count))) {
        fprintf(stderr, "get: %s\n", ecsdo_strerror(ret));
        return 1;
    }
    first = pos > 0xffff ? 0 : (uint16_t) pos;
    last = pos > 0xffff ? (uint16_t) (count ? count - 1 : 0) : first;
    if (pos > 0xffff && !count)
        return 0;

    items = calloc((size_t) (last - first + 1) * (size_t) n_sel,
            sizeof(*items));
    if (!items)
        return 1;
    for (p = first; ; p++) {
        for (k = 0; k < n_sel; k++) {
            items[n].position = p;
            items[n++].selector = sel[k];
        }
        if (p == last)
            break;
    }

    ret = ecsdo_get_items(ctx, items, n);
    if (json) {
        ecsdo_items_json(items, n, stdout);
    } else {
        for (i = 0; i < n; i++) {
            const ecsdo_item_t *it = &items[i];
            const ecsdo_value_t *v = &it->value;
            const char *tn = ecsdo_type_name(it->data_type);

            printf("slave %u  %-28s ", it->position, it->selector);
            if (it->result && !v->data) {
                printf("ERROR %s", ecsdo_util_strerror(it->result));
                if (it->result == ECSDO_ERR_ABORT)
                    printf(" 0x%08x", it->abort_code);
                printf("\n");
                continue;
            }
            printf("0x%04x:%02x %-14s = ", it->index, it->subindex,
                    tn ? tn : "?");
            if (it->result)
                printf("(%s) ", ecsdo_util_strerror(it->result));
            switch (it->result ? ECSDO_VALUE_RAW : v->kind) {
                case ECSDO_VALUE_UNSIGNED:
                    printf("0x%0*llx %llu", (int) (2 * v->size),
                            (unsigned long long) v->u,
                            (unsigned long long) v->u);
                    break;
                case ECSDO_VALUE_SIGNED:
                    printf("%lld", (long long) v->i);
                    break;
                case ECSDO_VALUE_REAL:
                    printf("%g", v->f);
                    break;
                case ECSDO_VALUE_STRING:
                    printf("\"%.*s\"", (int) v->str_len, v->str);
                    break;
                default:
                    for (size_t b = 0; b < v->size; b++)
                        printf("%02x%s", v->data[b],
                                b + 1 < v->size ? " " : "");
                    break;
            }
            printf("\n");
        }
    }
    free(items);
    return ret ? 1 : 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-m MASTER] -p POS sdos\n"
            "       %s [-m MASTER] -p POS upload [-t raw] IDX SUB\n"
            "       %s [-m MASTER] scan                  (JSON dictionary)\n"
            "       %s [-m MASTER] [-p POS] get [-j] SELECTOR...\n"
            "           SELECTOR: \"Name\", \"Name:SUB\", \"Name/Entry\","
            " \"Entry\", \"0xIDX:SUB\"\n",
            prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    unsigned long master = 0, pos = 0x10000, idx, sub;
    ecsdo_ctx_t *ctx;
    int i = 1, ret, rc;

    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-m"))
            master = strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "-p"))
            pos = strtoul(argv[i + 1], NULL, 0);
        else
            break;
    }
    if (i >= argc || (pos > 0xffff && strcmp(argv[i], "scan")
                && strcmp(argv[i], "get"))) {
        usage(argv[0]);
        return 2;
    }

    ret = ecsdo_open(&ctx, (unsigned int) master);
    if (ret) {
        fprintf(stderr, "open: %s\n", ecsdo_strerror(ret));
        return 1;
    }

    if (!strcmp(argv[i], "scan") && i + 1 == argc) {
        rc = cmd_scan(ctx, (unsigned int) master);
    } else if (!strcmp(argv[i], "get") && i + 1 < argc) {
        int json = !strcmp(argv[i + 1], "-j");
        int first = i + 1 + json;

        if (first >= argc) {
            usage(argv[0]);
            rc = 2;
        } else {
            rc = cmd_get(ctx, pos, json, argv + first, argc - first);
        }
    } else if (!strcmp(argv[i], "sdos") && i + 1 == argc) {
        rc = cmd_sdos(ctx, (uint16_t) pos);
    } else if (!strcmp(argv[i], "upload") && (i + 3 == argc
                || (i + 5 == argc && !strcmp(argv[i + 1], "-t")
                    && !strcmp(argv[i + 2], "raw")))) {
        int raw = i + 5 == argc;

        idx = strtoul(argv[argc - 2], NULL, 0);
        sub = strtoul(argv[argc - 1], NULL, 0);
        if (idx > 0xffff || sub > 0xff) {
            usage(argv[0]);
            rc = 2;
        } else {
            rc = cmd_upload(ctx, (uint16_t) pos, (uint16_t) idx,
                    (uint8_t) sub, raw);
        }
    } else {
        usage(argv[0]);
        rc = 2;
    }

    ecsdo_close(ctx);
    return rc;
}
