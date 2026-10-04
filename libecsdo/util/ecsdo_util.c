/*
 * libecsdo_util: dictionary JSON dump, values by name, items JSON.
 * Uses only the public libecsdo API.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ecsdo/ecsdo_util.h"

const char *ecsdo_util_strerror(int code)
{
    switch (code) {
        case ECSDO_ERR_NOT_FOUND: return "no SDO entry matches the selector";
        case ECSDO_ERR_AMBIGUOUS:
            return "selector matches several entries (add :SUB or /name)";
        case ECSDO_ERR_NO_DICT:
            return "slave has no complete dictionary (use 0xIDX:SUB)";
        case ECSDO_ERR_BAD_SELECTOR: return "selector syntax error";
        case ECSDO_ERR_WRITE: return "writing output failed";
        default: return ecsdo_strerror(code);
    }
}

static const char *status_name(ecsdo_slave_status_t s)
{
    switch (s) {
        case ECSDO_SLAVE_OK: return "OK";
        case ECSDO_SLAVE_NO_COE: return "NO_COE";
        case ECSDO_SLAVE_NO_SDO_INFO: return "NO_SDO_INFO";
        case ECSDO_SLAVE_NOT_READY: return "NOT_READY";
        case ECSDO_SLAVE_ERROR: return "ERROR";
    }
    return "?";
}

/* ------------------------------------------------------------------------
 * JSON output helpers
 * ---------------------------------------------------------------------- */

/** JSON string of n bytes. Slave strings are bytes, not necessarily UTF-8:
 *  non-printable bytes are written as \u00XX (Latin-1 code points). */
static void json_bytes(FILE *f, const void *data, size_t n)
{
    const unsigned char *s = data;
    size_t i;

    fputc('"', f);
    for (i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] >= 0x7f || s[i] == '"' || s[i] == '\\')
            fprintf(f, "\\u%04x", s[i]);
        else
            fputc(s[i], f);
    }
    fputc('"', f);
}

static void json_str(FILE *f, const char *s)
{
    json_bytes(f, s, strlen(s));
}

static void json_access(FILE *f, const uint8_t *a)
{
    fprintf(f, "[%s, %s, %s]", a[0] ? "true" : "false",
            a[1] ? "true" : "false", a[2] ? "true" : "false");
}

static void json_type_name(FILE *f, uint16_t data_type)
{
    const char *tn = ecsdo_type_name(data_type);

    if (tn)
        json_str(f, tn);
    else
        fputs("null", f);
}

static int finish(FILE *f)
{
    if (fflush(f) || ferror(f))
        return ECSDO_ERR_WRITE;
    return ECSDO_OK;
}

/* ------------------------------------------------------------------------
 * Dictionary dump
 * ---------------------------------------------------------------------- */

static void json_slave(FILE *f, const ecsdo_slave_t *s)
{
    size_t i, j;

    fprintf(f, "  {\"position\": %u, \"alias\": %u, \"vendor_id\": "
            "\"0x%08x\", \"product_code\": \"0x%08x\", \"revision\": "
            "\"0x%08x\", \"serial\": \"0x%08x\",\n   \"name\": ",
            s->position, s->alias, s->vendor_id, s->product_code,
            s->revision, s->serial);
    json_str(f, s->name);
    fprintf(f, ", \"al_state\": %u, \"coe\": %s, \"sdo_info\": %s, "
            "\"status\": \"%s\", \"errno\": %d,\n   \"objects\": [",
            s->al_state, s->coe ? "true" : "false",
            s->sdo_info ? "true" : "false",
            status_name(s->status),
            s->sys_errno);

    for (i = 0; i < s->object_count; i++) {
        const ecsdo_object_t *o = &s->objects[i];

        fprintf(f, "%s\n    {\"index\": \"0x%04x\", \"max_subindex\": %u, "
                "\"name\": ", i ? "," : "", o->index, o->max_subindex);
        json_str(f, o->name);
        fputs(", \"entries\": [", f);
        for (j = 0; j < o->entry_count; j++) {
            const ecsdo_entry_t *e = &o->entries[j];

            fprintf(f, "%s\n     {\"subindex\": %u, \"data_type\": "
                    "\"0x%04x\", \"type_name\": ", j ? "," : "",
                    e->subindex, e->data_type);
            json_type_name(f, e->data_type);
            fprintf(f, ", \"bit_length\": %u, \"read_access\": ",
                    e->bit_length);
            json_access(f, e->read_access);
            fputs(", \"write_access\": ", f);
            json_access(f, e->write_access);
            fputs(", \"description\": ", f);
            json_str(f, e->description);
            fputc('}', f);
        }
        fputs("]}", f);
    }
    fputs(s->object_count ? "\n   ]}" : "]}", f);
}

int ecsdo_dump_dict_json(ecsdo_ctx_t *ctx, unsigned int master_index,
        FILE *f)
{
    ecsdo_slave_t *s;
    uint16_t count, i;
    int ret;

    if (!ctx || !f)
        return ECSDO_ERR_INVALID_ARG;
    ret = ecsdo_slave_count(ctx, &count);
    if (ret)
        return ret;

    fprintf(f, "{\"format\": \"ecsdo-dict\", \"version\": 1, \"master\": %u,"
            " \"slaves\": [\n", master_index);
    for (i = 0; i < count; i++) {
        ret = ecsdo_read_dict(ctx, i, &s);
        if (ret)
            return ret; /* bus changed or fatal */
        if (i)
            fputs(",\n", f);
        json_slave(f, s);
        ecsdo_free_slave(s);
    }
    fputs("\n]}\n", f);
    return finish(f);
}

/* ------------------------------------------------------------------------
 * Selectors
 * ---------------------------------------------------------------------- */

/** Next significant character for loose comparison; 0 at the end. */
static char loose_next(const char **p, const char *end)
{
    char c;

    while (*p < end && ((c = **p) == ' ' || c == '_' || c == '-'
                || c == '\t'))
        (*p)++;
    if (*p >= end)
        return 0;
    c = *(*p)++;
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

/** Loose equality of name (NUL-terminated) and pat[0..len). */
static int loose_eq(const char *name, const char *pat, size_t len)
{
    const char *a = name, *a_end = name + strlen(name);
    const char *b = pat, *b_end = pat + len;
    char ca, cb;

    do {
        ca = loose_next(&a, a_end);
        cb = loose_next(&b, b_end);
        if (ca != cb)
            return 0;
    } while (ca);
    return 1;
}

static int parse_uint(const char *s, size_t len, unsigned long max,
        unsigned long *out)
{
    char tmp[16], *end;

    while (len && *s == ' ') {
        s++;
        len--;
    }
    while (len && s[len - 1] == ' ')
        len--;
    if (!len || len >= sizeof(tmp) || *s < '0' || *s > '9')
        return -1;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    errno = 0;
    *out = strtoul(tmp, &end, 0);
    return (errno || *end || *out > max) ? -1 : 0;
}

typedef struct {
    int by_index;              /* "0xIDX[:SUB]" */
    unsigned long index;
    int has_sub;               /* ":SUB" given */
    unsigned long sub;
    const char *name;          /* object name or entry description */
    size_t name_len;
    const char *desc;          /* "/description" part */
    size_t desc_len;
} selector_t;

static int parse_selector(const char *sel, selector_t *p)
{
    const char *colon, *slash, *end;

    memset(p, 0, sizeof(*p));
    if (!sel || !*sel)
        return ECSDO_ERR_BAD_SELECTOR;
    end = sel + strlen(sel);

    if (sel[0] == '0' && (sel[1] == 'x' || sel[1] == 'X')) {
        colon = strchr(sel, ':');
        p->by_index = 1;
        if (parse_uint(sel, (size_t) ((colon ? colon : end) - sel), 0xffff,
                    &p->index) || !p->index)
            return ECSDO_ERR_BAD_SELECTOR;
        if (colon && parse_uint(colon + 1, (size_t) (end - colon - 1), 0xff,
                    &p->sub))
            return ECSDO_ERR_BAD_SELECTOR;
        p->has_sub = 1; /* default subindex 0 */
        return ECSDO_OK;
    }

    p->name = sel;
    p->name_len = (size_t) (end - sel);
    if ((slash = strchr(sel, '/'))) {
        p->name_len = (size_t) (slash - sel);
        p->desc = slash + 1;
        p->desc_len = (size_t) (end - slash - 1);
        if (!p->desc_len)
            return ECSDO_ERR_BAD_SELECTOR;
    } else if ((colon = strrchr(sel, ':'))
            && !parse_uint(colon + 1, (size_t) (end - colon - 1), 0xff,
                &p->sub)) {
        /* "Name:SUB" (a trailing ':' that is no number stays in the name) */
        p->has_sub = 1;
        p->name_len = (size_t) (colon - sel);
    }
    if (!p->name_len)
        return ECSDO_ERR_BAD_SELECTOR;
    return ECSDO_OK;
}

static const ecsdo_entry_t *entry_by_sub(const ecsdo_object_t *o,
        unsigned long sub)
{
    size_t j;

    for (j = 0; j < o->entry_count; j++) {
        if (o->entries[j].subindex == sub)
            return &o->entries[j];
    }
    return NULL;
}

/** Find the entry for a selector. Returns ECSDO_OK, NOT_FOUND or
 *  AMBIGUOUS. */
static int resolve(const ecsdo_slave_t *s, const selector_t *p,
        const ecsdo_object_t **obj, const ecsdo_entry_t **ent)
{
    const ecsdo_object_t *fo = NULL;
    const ecsdo_entry_t *fe = NULL;
    size_t i, j, hits = 0;

    *obj = NULL;
    *ent = NULL;

    /* 1. Object name (optionally with :SUB or /description). */
    for (i = 0; i < s->object_count; i++) {
        const ecsdo_object_t *o = &s->objects[i];
        const ecsdo_entry_t *e = NULL;

        if (!loose_eq(o->name, p->name, p->name_len))
            continue;
        if (p->has_sub) {
            e = entry_by_sub(o, p->sub);
        } else if (p->desc) {
            for (j = 0; j < o->entry_count; j++) {
                if (loose_eq(o->entries[j].description, p->desc,
                            p->desc_len)) {
                    if (e)
                        return ECSDO_ERR_AMBIGUOUS;
                    e = &o->entries[j];
                }
            }
        } else if (o->entry_count == 1) {
            e = &o->entries[0];
        } else if (o->max_subindex == 0) {
            e = entry_by_sub(o, 0);
        } else {
            return ECSDO_ERR_AMBIGUOUS; /* record/array: which entry? */
        }
        if (!e)
            continue;
        if (hits++)
            return ECSDO_ERR_AMBIGUOUS;
        fo = o;
        fe = e;
    }
    if (hits) {
        *obj = fo;
        *ent = fe;
        return ECSDO_OK;
    }
    if (p->has_sub || p->desc)
        return ECSDO_ERR_NOT_FOUND;

    /* 2. Entry description anywhere. */
    for (i = 0; i < s->object_count; i++) {
        for (j = 0; j < s->objects[i].entry_count; j++) {
            if (!loose_eq(s->objects[i].entries[j].description, p->name,
                        p->name_len))
                continue;
            if (hits++)
                return ECSDO_ERR_AMBIGUOUS;
            fo = &s->objects[i];
            fe = &s->objects[i].entries[j];
        }
    }
    if (!hits)
        return ECSDO_ERR_NOT_FOUND;
    *obj = fo;
    *ent = fe;
    return ECSDO_OK;
}

/* ------------------------------------------------------------------------
 * Items
 * ---------------------------------------------------------------------- */

/** Dictionary cache: one read per slave position. */
typedef struct {
    uint16_t position;
    int ret;                   /* ecsdo_read_dict() result */
    ecsdo_slave_t *slave;
} dict_cache_t;

static const dict_cache_t *get_dict(ecsdo_ctx_t *ctx, dict_cache_t *cache,
        size_t *n, uint16_t position)
{
    size_t i;

    for (i = 0; i < *n; i++) {
        if (cache[i].position == position)
            return &cache[i];
    }
    cache[*n].position = position;
    cache[*n].slave = NULL;
    cache[*n].ret = ecsdo_read_dict(ctx, position, &cache[*n].slave);
    return &cache[(*n)++];
}

/** Resolve it->selector in dictionary s (may be NULL for a slave without
 *  one): fills index/subindex/names/type/access. Returns the result code. */
static int resolve_item(const ecsdo_slave_t *s, ecsdo_item_t *it)
{
    const ecsdo_object_t *o = NULL;
    const ecsdo_entry_t *e = NULL;
    selector_t p;
    size_t i;
    int ret;

    ret = parse_selector(it->selector, &p);
    if (ret)
        return ret;

    if (p.by_index) {
        /* Dictionary optional: use it for type and names if present. */
        it->index = (uint16_t) p.index;
        it->subindex = (uint8_t) p.sub;
        for (i = 0; s && i < s->object_count; i++) {
            if (s->objects[i].index == it->index) {
                o = &s->objects[i];
                e = entry_by_sub(o, it->subindex);
                break;
            }
        }
    } else {
        if (!s || s->status != ECSDO_SLAVE_OK)
            return ECSDO_ERR_NO_DICT;
        ret = resolve(s, &p, &o, &e);
        if (ret)
            return ret;
        it->index = o->index;
        it->subindex = e->subindex;
    }

    if (o)
        memcpy(it->object_name, o->name, sizeof(it->object_name));
    if (e) {
        memcpy(it->description, e->description, sizeof(it->description));
        it->data_type = e->data_type;
        it->bit_length = e->bit_length;
        memcpy(it->read_access, e->read_access, sizeof(it->read_access));
        memcpy(it->write_access, e->write_access, sizeof(it->write_access));
    }
    return ECSDO_OK;
}

static void get_item(ecsdo_ctx_t *ctx, ecsdo_item_t *it,
        const dict_cache_t *dc)
{
    size_t size;
    int ret;

    if (dc->ret) {                       /* no slave, ioctl error, ... */
        it->result = dc->ret;
        return;
    }
    ret = resolve_item(dc->slave, it);
    if (ret) {
        it->result = ret;
        return;
    }

    ret = ecsdo_upload(ctx, it->position, it->index, it->subindex, it->buf,
            sizeof(it->buf), &size, &it->abort_code);
    if (ret) {
        it->result = ret;
        return;
    }
    /* Type check and decoding with the dictionary's type. */
    it->result = ecsdo_decode(it->data_type, it->buf, size, &it->value);
    it->value.bit_length = it->bit_length;
}

int ecsdo_get_items(ecsdo_ctx_t *ctx, ecsdo_item_t *items, size_t count)
{
    dict_cache_t *cache;
    size_t n = 0, i, failed = 0;

    if (!ctx || (count && !items))
        return ECSDO_ERR_INVALID_ARG;
    cache = calloc(count ? count : 1, sizeof(*cache));
    if (!cache)
        return ECSDO_ERR_NO_MEMORY;

    for (i = 0; i < count; i++) {
        ecsdo_item_t *it = &items[i];
        uint16_t position = it->position;
        const char *selector = it->selector;

        /* Reset outputs, keep inputs. */
        memset(it, 0, sizeof(*it));
        it->position = position;
        it->selector = selector;

        get_item(ctx, it, get_dict(ctx, cache, &n, position));
        if (it->result)
            failed++;
    }

    for (i = 0; i < n; i++)
        ecsdo_free_slave(cache[i].slave);
    free(cache);
    return (int) failed;
}

int ecsdo_items_json(const ecsdo_item_t *items, size_t count, FILE *f)
{
    size_t i, k;

    if (!f || (count && !items))
        return ECSDO_ERR_INVALID_ARG;

    fputs("{\"format\": \"ecsdo-items\", \"version\": 1, \"items\": [", f);
    for (i = 0; i < count; i++) {
        const ecsdo_item_t *it = &items[i];
        const ecsdo_value_t *v = &it->value;

        fprintf(f, "%s\n {\"position\": %u, \"selector\": ", i ? "," : "",
                it->position);
        json_str(f, it->selector ? it->selector : "");
        fprintf(f, ", \"result\": %d, \"error\": ", it->result);
        if (it->result)
            json_str(f, ecsdo_util_strerror(it->result));
        else
            fputs("null", f);
        fprintf(f, ",\n  \"index\": \"0x%04x\", \"subindex\": %u, "
                "\"object_name\": ", it->index, it->subindex);
        json_str(f, it->object_name);
        fputs(", \"description\": ", f);
        json_str(f, it->description);
        fprintf(f, ", \"data_type\": \"0x%04x\", \"type_name\": ",
                it->data_type);
        json_type_name(f, it->data_type);
        fprintf(f, ", \"bit_length\": %u,\n  \"abort_code\": \"0x%08x\", "
                "\"value_hex\": ", it->bit_length, it->abort_code);
        if (v->data) {
            fputc('"', f);
            for (k = 0; k < v->size; k++)
                fprintf(f, "%02x", v->data[k]);
            fputc('"', f);
        } else {
            fputs("null", f);
        }
        fputs(", \"value\": ", f);
        switch (it->result ? ECSDO_VALUE_RAW : v->kind) {
            case ECSDO_VALUE_UNSIGNED:
                fprintf(f, "%llu", (unsigned long long) v->u);
                break;
            case ECSDO_VALUE_SIGNED:
                fprintf(f, "%lld", (long long) v->i);
                break;
            case ECSDO_VALUE_REAL:
                if (isfinite(v->f))
                    fprintf(f, "%.17g", v->f);
                else
                    fputs("null", f);
                break;
            case ECSDO_VALUE_STRING:
                json_bytes(f, v->str, v->str_len);
                break;
            default:
                fputs("null", f);
                break;
        }
        fputc('}', f);
    }
    fputs(count ? "\n]}\n" : "]}\n", f);
    return finish(f);
}
