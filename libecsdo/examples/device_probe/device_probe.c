/*
 * device_probe - first step of device classification: decide per slave
 * which path it takes (infrastructure / non_coe / cia402 / mdp /
 * other_profile), log every decision, and dump what an LLM needs to map
 * the device's objects and PDO entries to roles as JSON, one file per
 * slave. No LLM call here.
 *
 * Read-only: no master reservation, no activation. PRE-OP is enough.
 *
 *   device_probe [-m MASTER] [-o DIR] [-v VENDOR_IDS_JSON] [-t WAIT_MS]
 *
 * Writes DIR/slave_<pos>.json (default DIR "dump"). See README.md here.
 */

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo/ecsdo_sii.h"

#ifndef DEVICE_PROBE_VENDOR_IDS
#define DEVICE_PROBE_VENDOR_IDS "vendor_ids.json"
#endif

#define VENDOR_OBJECTS_MAX 150
#define OBJECTS_MAX 300                /* od.objects (other_profile) */
#define STR_BUF 256

/* Device type 0x1000 profiles. */
#define PROFILE_CIA402 402
#define PROFILE_MDP 5001

/* ------------------------------------------------------------------------
 * Collected data
 * ---------------------------------------------------------------------- */

typedef struct {
    char key[32];                    /* JSON key the read belonged to */
    uint16_t index;
    uint8_t subindex;
    int code;                        /* ECSDO_ERR_* */
    uint32_t abort_code;
} probe_err_t;

typedef struct {
    uint16_t index;                  /* 0 = gap */
    uint8_t subindex;
    uint8_t bit_length;
} map_entry_t;

typedef struct {
    uint16_t index;                  /* 0x16xx / 0x1Axx */
    size_t entry_count;
    map_entry_t *entries;
} od_pdo_t;

typedef struct {
    int ok;                          /* read completely */
    size_t pdo_count;
    od_pdo_t *pdos;
} od_pdos_t;

typedef struct {
    int have;
    uint32_t value;
} opt_u32_t;

typedef struct {
    uint16_t position;
    const char *branch;

    ecsdo_slave_t *dict;             /* identity + dictionary */
    ecsdo_sii_info_t info;
    ecsdo_sii_t *sii;                /* NULL if the SII was unreadable */

    opt_u32_t device_type;
    opt_u32_t id_od[4];              /* 0x1018:01..04 */
    char *name_ver[3];               /* 0x1008, 0x1009, 0x100A */
    opt_u32_t drive_modes;
    od_pdos_t rx, tx;

    size_t err_count;
    probe_err_t *errs;
} probe_t;

static int fatal_not_writable;

static void add_err(probe_t *p, const char *key, uint16_t index,
        uint8_t subindex, int code, uint32_t abort_code)
{
    probe_err_t *e = realloc(p->errs, (p->err_count + 1) * sizeof(*e));

    if (!e)
        return; /* only the error report is lost */
    p->errs = e;
    e = &e[p->err_count++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->index = index;
    e->subindex = subindex;
    e->code = code;
    e->abort_code = abort_code;
}

/* ------------------------------------------------------------------------
 * SDO reads (raw upload, decoded here: works without SDO information)
 * ---------------------------------------------------------------------- */

/** Upload; on failure record it under key. Returns ECSDO_OK or error. */
static int upload(ecsdo_ctx_t *ctx, probe_t *p, const char *key,
        uint16_t index, uint8_t sub, uint8_t *buf, size_t size,
        size_t *len)
{
    uint32_t abort_code = 0;
    int ret;

    ret = ecsdo_upload(ctx, p->position, index, sub, buf, size, len,
            &abort_code);
    if (ret == ECSDO_ERR_NOT_WRITABLE)
        fatal_not_writable = 1;
    if (ret)
        add_err(p, key, index, sub, ret, abort_code);
    return ret;
}

/** Little-endian unsigned value of 1..4 bytes. */
static int upload_u32(ecsdo_ctx_t *ctx, probe_t *p, const char *key,
        uint16_t index, uint8_t sub, uint32_t *value)
{
    uint8_t buf[8];
    size_t len, i;
    int ret;

    ret = upload(ctx, p, key, index, sub, buf, sizeof(buf), &len);
    if (ret)
        return ret;
    if (!len || len > 4) {
        add_err(p, key, index, sub, ECSDO_ERR_TYPE_MISMATCH, 0);
        return ECSDO_ERR_TYPE_MISMATCH;
    }
    *value = 0;
    for (i = 0; i < len; i++)
        *value |= (uint32_t) buf[i] << (8 * i);
    return ECSDO_OK;
}

static void read_opt_u32(ecsdo_ctx_t *ctx, probe_t *p, const char *key,
        uint16_t index, uint8_t sub, opt_u32_t *out)
{
    out->have = !upload_u32(ctx, p, key, index, sub, &out->value);
}

/** VISIBLE_STRING without trailing NUL padding; NULL if not readable. */
static char *upload_string(ecsdo_ctx_t *ctx, probe_t *p, const char *key,
        uint16_t index)
{
    uint8_t buf[STR_BUF];
    size_t len;
    char *s;

    if (upload(ctx, p, key, index, 0, buf, sizeof(buf), &len))
        return NULL;
    while (len && !buf[len - 1])
        len--;
    if (!len)
        return NULL; /* empty counts as missing */
    s = malloc(len + 1);
    if (s) {
        memcpy(s, buf, len);
        s[len] = '\0';
    }
    return s;
}

/** Assignment object (0x1C12/0x1C13) and the mapping of every assigned
 *  PDO. Mapping value: index << 16 | subindex << 8 | bit length. */
static void read_od_pdos(ecsdo_ctx_t *ctx, probe_t *p, const char *key,
        uint16_t assign, od_pdos_t *out)
{
    uint32_t n, idx, cnt, map;
    uint32_t i, j;

    if (upload_u32(ctx, p, key, assign, 0, &n))
        return;
    if (n > 255)
        n = 255; /* subindexes are 8 bit */
    out->pdos = calloc(n ? n : 1, sizeof(*out->pdos));
    if (!out->pdos)
        return;
    for (i = 1; i <= n; i++) {
        od_pdo_t *pdo = &out->pdos[out->pdo_count];

        if (upload_u32(ctx, p, key, assign, (uint8_t) i, &idx))
            return;
        out->pdo_count++;
        pdo->index = (uint16_t) idx;
        if (upload_u32(ctx, p, key, pdo->index, 0, &cnt))
            return;
        if (cnt > 255)
            cnt = 255;
        pdo->entries = calloc(cnt ? cnt : 1, sizeof(*pdo->entries));
        if (!pdo->entries)
            return;
        for (j = 1; j <= cnt; j++) {
            map_entry_t *e = &pdo->entries[pdo->entry_count];

            if (upload_u32(ctx, p, key, pdo->index, (uint8_t) j, &map))
                return;
            e->index = (uint16_t) (map >> 16);
            e->subindex = (uint8_t) (map >> 8);
            e->bit_length = (uint8_t) map;
            pdo->entry_count++;
        }
    }
    out->ok = 1;
}

static void free_od_pdos(od_pdos_t *o)
{
    size_t i;

    for (i = 0; i < o->pdo_count; i++)
        free(o->pdos[i].entries);
    free(o->pdos);
}

/* ------------------------------------------------------------------------
 * Vendor names (vendor_ids.json: {"0x00000002": "Beckhoff ...", ...})
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    char *name;
} vendor_t;

static vendor_t *vendors;
static size_t vendor_count;

/** JSON string at *p (after the opening quote); simple escapes only. */
static char *parse_json_string(const char **p)
{
    const char *s = *p;
    char *out = malloc(strlen(s) + 1), *o = out;

    if (!out)
        return NULL;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
    *p = *s ? s + 1 : s;
    return out;
}

static void load_vendors(const char *path)
{
    char *text, *key, *val;
    const char *p;
    FILE *f;
    long n;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "warning: %s: %s (vendor names will be null)\n",
                path, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    text = n > 0 ? malloc((size_t) n + 1) : NULL;
    if (!text || fread(text, 1, (size_t) n, f) != (size_t) n) {
        free(text);
        fclose(f);
        return;
    }
    text[n] = '\0';
    fclose(f);

    /* Flat object of "key": "value" pairs. */
    for (p = text; (p = strchr(p, '"'));) {
        p++;
        key = parse_json_string(&p);
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
                || *p == ':')
            p++;
        if (*p != '"') {
            free(key);
            continue;
        }
        p++;
        val = parse_json_string(&p);
        if (key && val && key[0] != '_') {
            vendor_t *v = realloc(vendors,
                    (vendor_count + 1) * sizeof(*v));

            if (v) {
                vendors = v;
                vendors[vendor_count].id =
                    (uint32_t) strtoul(key, NULL, 0);
                vendors[vendor_count++].name = val;
                val = NULL;
            }
        }
        free(key);
        free(val);
    }
    free(text);
}

static const char *vendor_name(uint32_t id)
{
    size_t i;

    for (i = 0; i < vendor_count; i++)
        if (vendors[i].id == id)
            return vendors[i].name;
    return NULL;
}

/* ------------------------------------------------------------------------
 * JSON output
 * ---------------------------------------------------------------------- */

/** Slave strings are bytes, not necessarily UTF-8: escape non-ASCII. */
static void json_str(FILE *f, const char *s)
{
    const unsigned char *c = (const unsigned char *) s;

    fputc('"', f);
    for (; *c; c++) {
        if (*c < 0x20 || *c >= 0x7f || *c == '"' || *c == '\\')
            fprintf(f, "\\u%04x", *c);
        else
            fputc(*c, f);
    }
    fputc('"', f);
}

/** String or null if empty. */
static void json_str_or_null(FILE *f, const char *s)
{
    if (s && *s)
        json_str(f, s);
    else
        fputs("null", f);
}

static void json_type(FILE *f, uint16_t data_type)
{
    const char *tn = ecsdo_type_name(data_type);

    if (tn)
        json_str(f, tn);
    else if (data_type)
        fprintf(f, "\"0x%04x\"", data_type);
    else
        fputs("null", f);
}

static const ecsdo_object_t *find_object(const ecsdo_slave_t *s,
        uint16_t index)
{
    size_t i;

    for (i = 0; s && i < s->object_count; i++)
        if (s->objects[i].index == index)
            return &s->objects[i];
    return NULL;
}

static const ecsdo_entry_t *find_entry(const ecsdo_object_t *o,
        uint8_t sub)
{
    size_t i;

    for (i = 0; o && i < o->entry_count; i++)
        if (o->entries[i].subindex == sub)
            return &o->entries[i];
    return NULL;
}

/** "ro"/"rw"/"wo"/"none" if equal in all states, else "rwrw-w" style
 *  (PREOP, SAFEOP, OP) like `ethercat sdos`. */
static void json_access(FILE *f, const ecsdo_entry_t *e)
{
    static const char *const same[4] = {"none", "ro", "wo", "rw"};
    int st[ECSDO_ACCESS_COUNT], i;

    for (i = 0; i < ECSDO_ACCESS_COUNT; i++)
        st[i] = (e->read_access[i] ? 1 : 0) | (e->write_access[i] ? 2 : 0);
    if (st[0] == st[1] && st[1] == st[2]) {
        fprintf(f, "\"%s\"", same[st[0]]);
        return;
    }
    fputc('"', f);
    for (i = 0; i < ECSDO_ACCESS_COUNT; i++)
        fprintf(f, "%c%c", st[i] & 1 ? 'r' : '-', st[i] & 2 ? 'w' : '-');
    fputc('"', f);
}

static void json_errors(FILE *f, const probe_t *p)
{
    size_t i;

    fputs("  \"errors\": [", f);
    for (i = 0; i < p->err_count; i++) {
        const probe_err_t *e = &p->errs[i];

        fprintf(f, "%s\n    {\"key\": \"%s\", \"index\": \"0x%04x\", "
                "\"subindex\": %u, ", i ? "," : "", e->key, e->index,
                e->subindex);
        if (e->code == ECSDO_ERR_ABORT) {
            const char *msg = ecsdo_abort_string(e->abort_code);

            fprintf(f, "\"abort_code\": \"0x%08x\", \"message\": ",
                    e->abort_code);
            json_str_or_null(f, msg);
        } else {
            fputs("\"error\": ", f);
            json_str(f, ecsdo_strerror(e->code));
        }
        fputc('}', f);
    }
    fputs(p->err_count ? "\n  ]\n" : "]\n", f);
}

/* 1-5 */

static void json_sii_basic(FILE *f, const probe_t *p, int with_name)
{
    static const struct {
        uint16_t bit;
        const char *name;
    } mbx[] = {
        {ECSDO_MBOX_AOE, "AoE"}, {ECSDO_MBOX_EOE, "EoE"},
        {ECSDO_MBOX_COE, "CoE"}, {ECSDO_MBOX_FOE, "FoE"},
        {ECSDO_MBOX_SOE, "SoE"}, {ECSDO_MBOX_VOE, "VoE"},
    };
    const ecsdo_slave_t *s = p->dict;
    int first = 1;
    size_t i;

    fprintf(f, "  \"sii.identity\": {\"vendor_id\": \"0x%08x\", "
            "\"product_code\": \"0x%08x\", \"revision\": \"0x%08x\"},\n",
            s->vendor_id, s->product_code, s->revision);
    fputs("  \"sii.vendor_name\": ", f);
    json_str_or_null(f, vendor_name(s->vendor_id));
    fputs(",\n  \"sii.strings\": {", f);
    if (with_name) {
        fputs("\"name\": ", f);
        json_str_or_null(f, s->name);
        fputs(", ", f);
    }
    fputs("\"order\": ", f);
    json_str_or_null(f, p->info.order);
    fputs(", \"group\": ", f);
    json_str_or_null(f, p->info.group);
    fputs("},\n  \"sii.mailbox\": [", f);
    for (i = 0; i < sizeof(mbx) / sizeof(mbx[0]); i++) {
        if (p->info.mailbox_protocols & mbx[i].bit) {
            fprintf(f, "%s\"%s\"", first ? "" : ", ", mbx[i].name);
            first = 0;
        }
    }
    fputs("],\n  \"sii.sdo_info\": ", f);
    if (!p->info.has_general_category)
        fputs("null", f);
    else
        fputs(p->info.enable_sdo_info ? "true" : "false", f);
    fputs(",\n  \"sii.coe_details\": ", f);
    if (!p->info.has_general_category)
        fputs("null", f);
    else
        fprintf(f, "{\"pdo_assign\": %s, \"pdo_config\": %s}",
                p->info.enable_pdo_assign ? "true" : "false",
                p->info.enable_pdo_configuration ? "true" : "false");
    fputs(",\n", f);
}

/* 6 */

static void json_device_type(FILE *f, const probe_t *p)
{
    static const char *const types402[] = {
        NULL, "frequency_converter", "servo_drive", "stepper_motor",
        "multiple_axes",
    };
    uint32_t v = p->device_type.value;
    unsigned int profile = v & 0xffff, type = (v >> 16) & 0xff;

    if (!p->device_type.have)
        return;
    fprintf(f, "  \"od.device_type\": {\"raw\": \"0x%08x\", "
            "\"profile\": %u, ", v, profile);
    if (profile == PROFILE_CIA402) {
        fprintf(f, "\"type_code\": \"0x%02x\", \"type\": ", type);
        json_str_or_null(f, type < sizeof(types402) / sizeof(types402[0])
                ? types402[type] : NULL);
    } else {
        fprintf(f, "\"additional_info\": \"0x%04x\"", v >> 16);
    }
    fputs("},\n", f);
}

/* 7: vendor, product, revision must match; serial only if the SII has
 *    one (SII serials are often 0). Serial values are not dumped. */

static void json_identity(FILE *f, const probe_t *p)
{
    const ecsdo_slave_t *s = p->dict;
    const uint32_t sii[3] = {s->vendor_id, s->product_code, s->revision};
    static const char *const names[3] = {
        "vendor_id", "product_code", "revision",
    };
    int differs[3] = {0, 0, 0}, serial_differs = 0, i, match = 1;

    for (i = 0; i < 3; i++) {
        if (!p->id_od[i].have) {
            fputs("  \"od.identity_match\": null,\n", f);
            return;
        }
        differs[i] = p->id_od[i].value != sii[i];
        if (differs[i])
            match = 0;
    }
    if (s->serial && p->id_od[3].have && p->id_od[3].value != s->serial) {
        serial_differs = 1;
        match = 0;
    }
    fprintf(f, "  \"od.identity_match\": %s,\n", match ? "true" : "false");
    if (match)
        return;
    fputs("  \"od.identity_mismatch\": {", f);
    for (i = 0; i < 3; i++)
        if (differs[i])
            fprintf(f, "\"%s\": \"0x%08x\", ", names[i], p->id_od[i].value);
    fprintf(f, "\"serial_differs\": %s},\n",
            serial_differs ? "true" : "false");
}

/* 8 */

static void json_name_ver(FILE *f, const probe_t *p)
{
    static const char *const keys[3] = {
        "device_name", "hardware_version", "software_version",
    };
    int i, first = 1;

    if (!p->name_ver[0] && !p->name_ver[1] && !p->name_ver[2])
        return;
    fputs("  \"od.name_ver\": {", f);
    for (i = 0; i < 3; i++) {
        if (!p->name_ver[i])
            continue;
        fprintf(f, "%s\"%s\": ", first ? "" : ", ", keys[i]);
        json_str(f, p->name_ver[i]);
        first = 0;
    }
    fputs("},\n", f);
}

/* 9 */

static void json_drive_modes(FILE *f, const probe_t *p)
{
    static const struct {
        int bit;
        const char *name;
    } modes[] = {
        {0, "PP"}, {1, "VL"}, {2, "PV"}, {3, "PT"}, {5, "HM"}, {6, "IP"},
        {7, "CSP"}, {8, "CSV"}, {9, "CST"},
    };
    int first = 1;
    size_t i;

    if (!p->drive_modes.have)
        return;
    fprintf(f, "  \"od.drive_modes\": {\"raw\": \"0x%08x\", \"modes\": [",
            p->drive_modes.value);
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (p->drive_modes.value & (1u << modes[i].bit)) {
            fprintf(f, "%s\"%s\"", first ? "" : ", ", modes[i].name);
            first = 0;
        }
    }
    fputs("]},\n", f);
}

/* 10, 11: names and types of the mapped objects from the dictionary;
 * without one (no SDO information), from the same entry in the SII PDOs,
 * marked "source": "sii". */

static const ecsdo_sii_pdo_entry_t *find_sii_entry(const ecsdo_sii_t *sii,
        uint16_t index, uint8_t sub)
{
    size_t i, j;

    for (i = 0; sii && i < sii->pdo_count; i++)
        for (j = 0; j < sii->pdos[i].entry_count; j++)
            if (sii->pdos[i].entries[j].index == index
                    && sii->pdos[i].entries[j].subindex == sub)
                return &sii->pdos[i].entries[j];
    return NULL;
}

static void json_mapped_entry(FILE *f, const probe_t *p,
        const map_entry_t *m)
{
    const ecsdo_sii_pdo_entry_t *se;
    const ecsdo_object_t *o;
    const ecsdo_entry_t *e;

    fprintf(f, "{\"index\": \"0x%04x\", \"subindex\": %u, \"bit_len\": %u",
            m->index, m->subindex, m->bit_length);
    if (!m->index) {
        fputs(", \"gap\": true}", f);
        return;
    }
    o = find_object(p->dict, m->index);
    e = find_entry(o, m->subindex);
    if (!o && (se = find_sii_entry(p->sii, m->index, m->subindex))) {
        fputs(", \"data_type\": ", f);
        json_type(f, se->data_type);
        fputs(", \"name\": ", f);
        json_str_or_null(f, se->name);
        fputs(", \"source\": \"sii\"}", f);
        return;
    }
    fputs(", \"data_type\": ", f);
    json_type(f, e ? e->data_type : 0);
    fputs(", \"name\": ", f);
    json_str_or_null(f, o ? o->name : NULL);
    if (m->subindex && e && e->description[0]) {
        fputs(", \"entry_name\": ", f);
        json_str(f, e->description);
    }
    fputc('}', f);
}

static void json_od_pdos(FILE *f, const probe_t *p, const char *key,
        const od_pdos_t *o)
{
    size_t i, j;

    if (!o->ok)
        return;
    fprintf(f, "  \"%s\": {\"basis\": \"assigned\", \"pdos\": [", key);
    for (i = 0; i < o->pdo_count; i++) {
        const od_pdo_t *pdo = &o->pdos[i];

        fprintf(f, "%s\n    {\"index\": \"0x%04x\", \"entries\": [",
                i ? "," : "", pdo->index);
        for (j = 0; j < pdo->entry_count; j++) {
            fputs(j ? ",\n      " : "\n      ", f);
            json_mapped_entry(f, p, &pdo->entries[j]);
        }
        fputs(pdo->entry_count ? "\n    ]}" : "]}", f);
    }
    fputs(o->pdo_count ? "\n  ]},\n" : "]},\n", f);
}

/* 12-14 */

static const char *dict_unavailable(const ecsdo_slave_t *s)
{
    switch (s->status) {
        case ECSDO_SLAVE_OK: return NULL;
        case ECSDO_SLAVE_NO_COE: return "no_coe";
        case ECSDO_SLAVE_NO_SDO_INFO: return "no_sdo_info";
        case ECSDO_SLAVE_NOT_READY: return "dict_not_ready";
        case ECSDO_SLAVE_ERROR: return "dict_error";
    }
    return "dict_error";
}

/** Object-level metadata: a single-value object gives its type and
 *  access; one with subindexes is "RECORD" (the master does not tell
 *  VAR/ARRAY/RECORD apart) with the entry count and subindex 1's type and
 *  access. */
static void json_object_meta(FILE *f, const ecsdo_object_t *o)
{
    const ecsdo_entry_t *e = NULL;
    size_t i, subs = 0;

    fprintf(f, "{\"index\": \"0x%04x\", \"name\": ", o->index);
    json_str_or_null(f, o->name);
    if (!o->max_subindex) {
        e = find_entry(o, 0);
        fputs(", \"data_type\": ", f);
        json_type(f, e ? e->data_type : 0);
    } else {
        for (i = 0; i < o->entry_count; i++) {
            if (!o->entries[i].subindex)
                continue;
            if (!e)
                e = &o->entries[i]; /* sub 1, or the first one there is */
            subs++;
        }
        fprintf(f, ", \"data_type\": \"RECORD\", \"entries\": %zu, "
                "\"sub1_type\": ", subs);
        json_type(f, e ? e->data_type : 0);
    }
    fputs(", \"access\": ", f);
    if (e)
        json_access(f, e);
    else
        fputs("null", f);
    fputc('}', f);
}

static void json_profile(FILE *f, const probe_t *p)
{
    const ecsdo_slave_t *s = p->dict;
    const char *why = dict_unavailable(s);
    size_t i, n, total;
    int axis, first;

    if (why) {
        fprintf(f, "  \"od.profile_axis0\": {\"unavailable\": \"%s\"},\n"
                "  \"od.profile_axes_extra\": {\"unavailable\": \"%s\"},\n"
                "  \"od.vendor\": {\"unavailable\": \"%s\"},\n",
                why, why, why);
        return;
    }

    fputs("  \"od.profile_axis0\": [", f);
    for (i = 0, n = 0; i < s->object_count; i++) {
        const ecsdo_object_t *o = &s->objects[i];

        if (o->index < 0x6000 || o->index > 0x67FF)
            continue;
        fputs(n++ ? ",\n    " : "\n    ", f);
        json_object_meta(f, o);
    }
    fputs(n ? "\n  ],\n" : "],\n", f);

    /* Axes 1..7: 0x6800 + 0x800 * (axis - 1), up to 0x9FFF. */
    fputs("  \"od.profile_axes_extra\": [", f);
    first = 1;
    for (axis = 1; axis <= 7; axis++) {
        unsigned int lo = 0x6000 + 0x800u * (unsigned int) axis;

        for (i = 0, n = 0; i < s->object_count; i++)
            if (s->objects[i].index >= lo
                    && s->objects[i].index <= lo + 0x7FF)
                n++;
        if (!n)
            continue;
        fprintf(f, "%s{\"axis\": %d, \"range\": \"0x%04X-0x%04X\", "
                "\"objects\": %zu}", first ? "" : ", ", axis, lo,
                lo + 0x7FF, n);
        first = 0;
    }
    fputs("],\n", f);

    fputs("  \"od.vendor\": {\"objects\": [", f);
    for (i = 0, n = 0, total = 0; i < s->object_count; i++) {
        const ecsdo_object_t *o = &s->objects[i];

        if (o->index < 0x2000 || o->index > 0x5FFF)
            continue;
        if (total++ >= VENDOR_OBJECTS_MAX)
            continue;
        fprintf(f, "%s{\"index\": \"0x%04x\", \"name\": ",
                n++ ? ",\n    " : "\n    ", o->index);
        json_str_or_null(f, o->name);
        fputc('}', f);
    }
    fprintf(f, "%s], \"total\": %zu, \"truncated\": %s},\n",
            n ? "\n  " : "", total,
            total > VENDOR_OBJECTS_MAX ? "true" : "false");
}

/* od.objects (other_profile): no profile to split the dictionary by, so
 * every object in 0x2000-0x9FFF with its metadata, up to OBJECTS_MAX. */

static void json_objects(FILE *f, const probe_t *p)
{
    const ecsdo_slave_t *s = p->dict;
    const char *why = dict_unavailable(s);
    size_t i, n, total;

    if (why) {
        fprintf(f, "  \"od.objects\": {\"unavailable\": \"%s\"},\n", why);
        return;
    }
    fputs("  \"od.objects\": {\"objects\": [", f);
    for (i = 0, n = 0, total = 0; i < s->object_count; i++) {
        const ecsdo_object_t *o = &s->objects[i];

        if (o->index < 0x2000 || o->index > 0x9FFF)
            continue;
        if (total++ >= OBJECTS_MAX)
            continue;
        fputs(n++ ? ",\n    " : "\n    ", f);
        json_object_meta(f, o);
    }
    fprintf(f, "%s], \"total\": %zu, \"truncated\": %s},\n",
            n ? "\n  " : "", total,
            total > OBJECTS_MAX ? "true" : "false");
}

/* sii.pdo: all SII PDOs of the given directions. Ones not assigned by
 * default (SM 0xFF) have "sm": null: they are the alternatives a device
 * with fixed PDO contents offers. */

static void json_sii_pdo_dir(FILE *f, const ecsdo_sii_t *sii,
        ecsdo_sii_pdo_dir_t dir)
{
    size_t i, j, n = 0;

    fputc('[', f);
    for (i = 0; i < sii->pdo_count; i++) {
        const ecsdo_sii_pdo_t *pdo = &sii->pdos[i];

        if (pdo->dir != dir)
            continue;
        fprintf(f, "%s\n      {\"index\": \"0x%04x\", \"name\": ",
                n++ ? "," : "", pdo->index);
        json_str_or_null(f, pdo->name);
        if (pdo->sync_index < 0)
            fputs(", \"sm\": null, \"entries\": [", f);
        else
            fprintf(f, ", \"sm\": %d, \"entries\": [", pdo->sync_index);
        for (j = 0; j < pdo->entry_count; j++) {
            const ecsdo_sii_pdo_entry_t *e = &pdo->entries[j];

            fprintf(f, "%s\n        {\"index\": \"0x%04x\", \"subindex\": "
                    "%u, \"bit_len\": %u", j ? "," : "", e->index,
                    e->subindex, e->bit_length);
            if (!e->index) {
                fputs(", \"gap\": true}", f);
                continue;
            }
            fputs(", \"data_type\": ", f);
            json_type(f, e->data_type);
            fputs(", \"name\": ", f);
            json_str_or_null(f, e->name);
            fputc('}', f);
        }
        fputs(pdo->entry_count ? "\n      ]}" : "]}", f);
    }
    fputs(n ? "\n    ]" : "]", f);
}

static void json_sii_pdo(FILE *f, const probe_t *p, int rx, int tx)
{
    if (!rx && !tx)
        return;
    if (!p->sii) {
        fputs("  \"sii.pdo\": {\"unavailable\": \"no_sii\"},\n", f);
        return;
    }
    fputs("  \"sii.pdo\": {", f);
    if (rx) {
        fputs("\"rxpdo\": ", f);
        json_sii_pdo_dir(f, p->sii, ECSDO_SII_RXPDO);
    }
    if (tx) {
        fputs(rx ? ",\n    \"txpdo\": " : "\"txpdo\": ", f);
        json_sii_pdo_dir(f, p->sii, ECSDO_SII_TXPDO);
    }
    fputs("},\n", f);
}

static int write_json(const char *path, const probe_t *p)
{
    int cia402 = !strcmp(p->branch, "cia402");
    int other = !strcmp(p->branch, "other_profile");
    int od_path = cia402 || other || !strcmp(p->branch, "mdp");
    /* PDO assignment writable but contents fixed: the SII PDOs are the
     * sets to choose from. */
    int fixed_sets = p->info.enable_pdo_assign
        && !p->info.enable_pdo_configuration;
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    fprintf(f, "{\n  \"position\": %u,\n  \"branch\": \"%s\",\n",
            p->position, p->branch);
    /* The OD device name replaces the SII one when it was read. */
    json_sii_basic(f, p, !((cia402 || other) && p->name_ver[0]));

    if (!strcmp(p->branch, "non_coe"))
        json_sii_pdo(f, p, 1, 1);
    if (od_path)
        json_device_type(f, p);
    if (cia402) {
        json_identity(f, p);
        json_name_ver(f, p);
        json_drive_modes(f, p);
        json_od_pdos(f, p, "od.rxpdo", &p->rx);
        json_od_pdos(f, p, "od.txpdo", &p->tx);
        json_profile(f, p);
        json_sii_pdo(f, p, fixed_sets || !p->rx.ok, fixed_sets || !p->tx.ok);
    }
    if (other) {
        /* No profile: the names in the PDOs and the dictionary are all
         * there is to go on, so give all of them. */
        json_identity(f, p);
        json_name_ver(f, p);
        json_od_pdos(f, p, "od.rxpdo", &p->rx);
        json_od_pdos(f, p, "od.txpdo", &p->tx);
        json_objects(f, p);
        json_sii_pdo(f, p, 1, 1);
    }
    if (!strcmp(p->branch, "mdp")) {
        /* No name/role conventions to lean on: give both the assigned
         * PDOs and the SII defaults (with PDO names). */
        json_od_pdos(f, p, "od.rxpdo", &p->rx);
        json_od_pdos(f, p, "od.txpdo", &p->tx);
        json_sii_pdo(f, p, 1, 1);
    }
    json_errors(f, p);
    fputs("}\n", f);
    if (fclose(f))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------------
 * Classification
 * ---------------------------------------------------------------------- */

static void log_step(const char *fmt, ...)
{
    va_list ap;

    fputs("  ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

/** 7, 8, 10, 11 (cia402, other_profile); 9 for cia402 only. */
static void read_od_device(ecsdo_ctx_t *ctx, probe_t *p, int cia402)
{
    static const char *const nv_key = "od.name_ver";
    int i;

    for (i = 0; i < 4; i++)
        read_opt_u32(ctx, p, "od.identity_match", 0x1018,
                (uint8_t) (i + 1), &p->id_od[i]);
    for (i = 0; i < 3; i++)
        p->name_ver[i] = upload_string(ctx, p, nv_key,
                (uint16_t) (0x1008 + i));
    if (cia402)
        read_opt_u32(ctx, p, "od.drive_modes", 0x6502, 0, &p->drive_modes);
    read_od_pdos(ctx, p, "od.rxpdo", 0x1C12, &p->rx);
    read_od_pdos(ctx, p, "od.txpdo", 0x1C13, &p->tx);
}

static int probe_slave(ecsdo_ctx_t *ctx, uint16_t pos, const char *dir)
{
    probe_t p;
    uint8_t *raw = NULL;
    size_t raw_size = 0, i;
    char path[512];
    int ret, pd;

    memset(&p, 0, sizeof(p));
    p.position = pos;

    if ((ret = ecsdo_read_dict(ctx, pos, &p.dict))
            || (ret = ecsdo_read_sii_info(ctx, pos, &p.info))) {
        fprintf(stderr, "slave %u: %s\n", pos, ecsdo_strerror(ret));
        ecsdo_free_slave(p.dict);
        return -1;
    }
    printf("[slave %u] \"%s\" vendor 0x%08x product 0x%08x rev 0x%08x, "
            "AL 0x%02x\n", pos, p.dict->name, p.dict->vendor_id,
            p.dict->product_code, p.dict->revision, p.dict->al_state);
    if ((p.dict->al_state & ECSDO_AL_STATE_MASK) < ECSDO_AL_PREOP
            || (p.dict->al_state & ECSDO_AL_STATE_MASK) == ECSDO_AL_BOOT)
        log_step("warning: not in PRE-OP or higher, SDO reads will fail");

    /* 1. Process data, from the SII PDO/SyncM categories. */
    ret = ecsdo_read_sii(ctx, pos, &raw, &raw_size);
    if (!ret && raw_size)
        ret = ecsdo_sii_parse(raw, raw_size, &p.sii);
    free(raw);
    if (ret || !p.sii) {
        add_err(&p, "sii.pdo", 0, 0, ret ? ret : ECSDO_ERR_IOCTL, 0);
        pd = -1;
        log_step("1. process data (SII): unknown, SII not readable (%s)"
                " -> continue", ret ? ecsdo_strerror(ret) : "empty");
    } else {
        size_t n_pdo = 0, n_sm = 0;

        for (i = 0; i < p.sii->pdo_count; i++)
            n_pdo += p.sii->pdos[i].sync_index >= 0
                && p.sii->pdos[i].entry_count;
        for (i = 0; i < p.sii->sync_count; i++)
            n_sm += p.sii->syncs[i].type == ECSDO_SII_SM_PD_OUT
                || p.sii->syncs[i].type == ECSDO_SII_SM_PD_IN;
        pd = ecsdo_sii_has_process_data(p.sii);
        log_step("1. process data (SII): %s (assigned PDOs %zu, PD SMs "
                "%zu%s)", pd ? "yes" : "no", n_pdo, n_sm,
                p.sii->incomplete ? ", SII malformed" : "");
    }
    if (!pd) {
        p.branch = "infrastructure";
        log_step("   -> infrastructure");
        goto write;
    }

    /* 2. CoE. */
    log_step("2. CoE: %s", p.dict->coe ? "yes" : "no");
    if (!p.dict->coe) {
        p.branch = "non_coe";
        log_step("   -> non_coe");
        goto write;
    }

    /* 3. SDO information: recorded only. */
    log_step("3. SDO info (SII General): %s",
            !p.info.has_general_category ? "unknown (no General category)"
            : p.info.enable_sdo_info ? "yes" : "no");

    /* 4. Device type, read with or without SDO information. */
    read_opt_u32(ctx, &p, "od.device_type", 0x1000, 0, &p.device_type);
    if (fatal_not_writable)
        goto out;
    if (!p.device_type.have) {
        p.branch = "other_profile";
        log_step("4. 0x1000: not readable -> other_profile");
    } else {
        unsigned int profile = p.device_type.value & 0xffff;

        p.branch = profile == PROFILE_CIA402 ? "cia402"
            : profile == PROFILE_MDP ? "mdp" : "other_profile";
        log_step("4. 0x1000 = 0x%08x, profile %u -> %s",
                p.device_type.value, profile, p.branch);
    }
    if (!strcmp(p.branch, "cia402") || !strcmp(p.branch, "other_profile"))
        read_od_device(ctx, &p, !strcmp(p.branch, "cia402"));
    else if (!strcmp(p.branch, "mdp")) {
        read_od_pdos(ctx, &p, "od.rxpdo", 0x1C12, &p.rx);
        read_od_pdos(ctx, &p, "od.txpdo", 0x1C13, &p.tx);
    }

write:
    snprintf(path, sizeof(path), "%s/slave_%u.json", dir, pos);
    if (write_json(path, &p)) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        ret = -1;
    } else {
        log_step("=> %s, %zu error(s) -> %s", p.branch, p.err_count, path);
        ret = 0;
    }

out:
    ecsdo_free_slave(p.dict);
    ecsdo_sii_free(p.sii);
    for (i = 0; i < 3; i++)
        free(p.name_ver[i]);
    free_od_pdos(&p.rx);
    free_od_pdos(&p.tx);
    free(p.errs);
    return fatal_not_writable ? -1 : ret;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-m MASTER] [-o DIR] [-v VENDOR_IDS_JSON] "
            "[-t WAIT_MS]\n", prog);
}

int main(int argc, char **argv)
{
    const char *dir = "dump", *vendor_file = DEVICE_PROBE_VENDOR_IDS;
    unsigned int master = 0, wait_ms = 30000;
    ecsdo_ctx_t *ctx;
    uint16_t count, pos;
    int opt, ret, failed = 0;
    size_t i;

    while ((opt = getopt(argc, argv, "m:o:v:t:h")) != -1) {
        switch (opt) {
            case 'm': master = (unsigned int) strtoul(optarg, NULL, 0); break;
            case 'o': dir = optarg; break;
            case 'v': vendor_file = optarg; break;
            case 't': wait_ms = (unsigned int) strtoul(optarg, NULL, 0);
                      break;
            default: usage(argv[0]); return 2;
        }
    }

    load_vendors(vendor_file);
    if (mkdir(dir, 0755) && errno != EEXIST) {
        fprintf(stderr, "%s: %s\n", dir, strerror(errno));
        return 1;
    }
    if ((ret = ecsdo_open(&ctx, master))) {
        fprintf(stderr, "open master %u: %s (%s)\n", master,
                ecsdo_strerror(ret), strerror(errno));
        return 1;
    }

    ret = ecsdo_wait_ready(ctx, wait_ms);
    if (ret == ECSDO_ERR_TIMEOUT)
        printf("warning: not all dictionaries complete after %u ms\n",
                wait_ms);
    else if (ret)
        fprintf(stderr, "wait_ready: %s\n", ecsdo_strerror(ret));

    if ((ret = ecsdo_slave_count(ctx, &count))) {
        fprintf(stderr, "slave count: %s\n", ecsdo_strerror(ret));
        ecsdo_close(ctx);
        return 1;
    }
    printf("%u slave(s)\n", count);
    for (pos = 0; pos < count; pos++) {
        if (probe_slave(ctx, pos, dir))
            failed++;
        if (fatal_not_writable) {
            fprintf(stderr, "SDO upload needs read-write access to "
                    "/dev/EtherCAT%u (run as root or fix permissions)\n",
                    master);
            break;
        }
    }

    ecsdo_close(ctx);
    for (i = 0; i < vendor_count; i++)
        free(vendors[i].name);
    free(vendors);
    return failed ? 1 : 0;
}
