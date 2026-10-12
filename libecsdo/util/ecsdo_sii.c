/*
 * SII category parser (see ecsdo_sii.h). Same walking rules as the IgH
 * master (fsm_slave_scan.c): category type masked with 0x7FFF, stop at
 * 0xFFFF or at the first category that does not fit.
 */

#include <stdlib.h>
#include <string.h>

#include "ecsdo/ecsdo_sii.h"

#define FIRST_CATEGORY_BYTE (0x0040 * 2)

#define CAT_STRINGS 0x000A
#define CAT_SYNCM   0x0029
#define CAT_TXPDO   0x0032
#define CAT_RXPDO   0x0033
#define CAT_END     0xFFFF

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

typedef struct {
    const uint8_t *data;             /* strings category, NULL if none */
    size_t size;
} strings_t;

/** Copy string number idx (1-based, 0 = none) to dst. */
static void get_string(const strings_t *st, uint8_t idx, char *dst)
{
    size_t pos = 1, n;
    unsigned int i;

    dst[0] = '\0';
    if (!idx || !st->data || !st->size || idx > st->data[0])
        return;
    for (i = 1; pos < st->size; i++) {
        n = st->data[pos++];
        if (pos + n > st->size)
            return;
        if (i == idx) {
            if (n > ECSDO_STRING_SIZE - 1)
                n = ECSDO_STRING_SIZE - 1;
            memcpy(dst, st->data + pos, n);
            dst[n] = '\0';
            return;
        }
        pos += n;
    }
}

/** Next category; 0 = got one, 1 = end of list, -1 = malformed. */
static int next_category(const uint8_t *data, size_t size, size_t *pos,
        uint16_t *type, const uint8_t **body, size_t *len)
{
    if (*pos + 2 > size)
        return -1;
    if (rd16(data + *pos) == CAT_END)
        return 1;
    if (*pos + 4 > size)
        return -1;
    *type = rd16(data + *pos) & 0x7FFF;
    *len = (size_t) rd16(data + *pos + 2) * 2;
    *body = data + *pos + 4;
    if (*pos + 4 + *len > size)
        return -1;
    *pos += 4 + *len;
    return 0;
}

static int parse_syncs(ecsdo_sii_t *sii, const uint8_t *p, size_t len)
{
    size_t n = len / 8, i;
    ecsdo_sii_sync_t *s;

    if (!n)
        return 0;
    s = realloc(sii->syncs, (sii->sync_count + n) * sizeof(*s));
    if (!s)
        return -1;
    sii->syncs = s;
    for (i = 0; i < n; i++, p += 8) {
        s = &sii->syncs[sii->sync_count++];
        s->start_address = rd16(p);
        s->default_size = rd16(p + 2);
        s->control = p[4];
        s->enable = p[6];
        s->type = p[7];
    }
    return 0;
}

/** Returns 0, -1 (no memory) or 1 (malformed, partial result kept). */
static int parse_pdos(ecsdo_sii_t *sii, const strings_t *st,
        const uint8_t *p, size_t len, ecsdo_sii_pdo_dir_t dir)
{
    const uint8_t *end = p + len;
    ecsdo_sii_pdo_t *pdos, *pdo;
    size_t i, count;

    while (end - p >= 8) {
        pdos = realloc(sii->pdos, (sii->pdo_count + 1) * sizeof(*pdos));
        if (!pdos)
            return -1;
        sii->pdos = pdos;
        pdo = &pdos[sii->pdo_count++];
        memset(pdo, 0, sizeof(*pdo));
        pdo->index = rd16(p);
        count = p[2];
        pdo->sync_index = (int8_t) p[3]; /* 0xFF = none, like the master */
        pdo->dir = dir;
        get_string(st, p[5], pdo->name);
        p += 8;

        if (!count)
            continue;
        if ((size_t) (end - p) < count * 8)
            return 1;
        pdo->entries = calloc(count, sizeof(*pdo->entries));
        if (!pdo->entries)
            return -1;
        for (i = 0; i < count; i++, p += 8) {
            ecsdo_sii_pdo_entry_t *e = &pdo->entries[i];

            e->index = rd16(p);
            e->subindex = p[2];
            get_string(st, p[3], e->name);
            e->data_type = p[4];
            e->bit_length = p[5];
        }
        pdo->entry_count = count;
    }
    return 0;
}

int ecsdo_sii_parse(const uint8_t *data, size_t size, ecsdo_sii_t **out)
{
    strings_t st = {NULL, 0};
    const uint8_t *body;
    ecsdo_sii_t *sii;
    uint16_t type;
    size_t pos, len;
    int ret;

    if (!out || (!data && size))
        return ECSDO_ERR_INVALID_ARG;
    *out = NULL;
    sii = calloc(1, sizeof(*sii));
    if (!sii)
        return ECSDO_ERR_NO_MEMORY;
    *out = sii;
    if (size <= FIRST_CATEGORY_BYTE)
        return ECSDO_OK;

    /* Strings first: other categories refer to them. */
    pos = FIRST_CATEGORY_BYTE;
    while (!(ret = next_category(data, size, &pos, &type, &body, &len))) {
        sii->has_categories = 1;
        if (type == CAT_STRINGS && !st.data) {
            st.data = body;
            st.size = len;
        }
    }
    if (ret < 0)
        sii->incomplete = 1;

    pos = FIRST_CATEGORY_BYTE;
    while (!next_category(data, size, &pos, &type, &body, &len)) {
        if (type == CAT_SYNCM)
            ret = parse_syncs(sii, body, len);
        else if (type == CAT_TXPDO || type == CAT_RXPDO)
            ret = parse_pdos(sii, &st, body, len,
                    type == CAT_TXPDO ? ECSDO_SII_TXPDO : ECSDO_SII_RXPDO);
        else
            ret = 0;
        if (ret < 0) {
            ecsdo_sii_free(sii);
            *out = NULL;
            return ECSDO_ERR_NO_MEMORY;
        }
        if (ret > 0) {
            sii->incomplete = 1;
            break;
        }
    }
    return ECSDO_OK;
}

void ecsdo_sii_free(ecsdo_sii_t *sii)
{
    size_t i;

    if (!sii)
        return;
    for (i = 0; i < sii->pdo_count; i++)
        free(sii->pdos[i].entries);
    free(sii->pdos);
    free(sii->syncs);
    free(sii);
}

int ecsdo_sii_has_process_data(const ecsdo_sii_t *sii)
{
    size_t i;

    if (!sii)
        return 0;
    for (i = 0; i < sii->pdo_count; i++)
        if (sii->pdos[i].entry_count)
            return 1;
    for (i = 0; i < sii->sync_count; i++)
        if (sii->syncs[i].type == ECSDO_SII_SM_PD_OUT
                || sii->syncs[i].type == ECSDO_SII_SM_PD_IN)
            return 1;
    return 0;
}
