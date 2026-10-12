/*
 * Builder for raw SII images in tests (fixed area zero, categories from
 * word 0x0040).
 */

#ifndef ECSDO_SII_IMG_H
#define ECSDO_SII_IMG_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t b[1024];
    size_t n;
    size_t cat;                      /* start of the open category */
} sii_img_t;

static inline void put8(sii_img_t *s, unsigned int v)
{
    s->b[s->n++] = (uint8_t) v;
}

static inline void put16(sii_img_t *s, unsigned int v)
{
    put8(s, v & 0xff);
    put8(s, v >> 8);
}

static inline void img_start(sii_img_t *s)
{
    memset(s, 0, sizeof(*s));
    s->n = 0x40 * 2;                 /* fixed area, zeros */
}

static inline void cat_begin(sii_img_t *s, unsigned int type)
{
    put16(s, type);
    put16(s, 0);                     /* size, patched in cat_end */
    s->cat = s->n;
}

static inline void cat_end(sii_img_t *s)
{
    size_t len = s->n - s->cat;

    if (len & 1) {
        put8(s, 0);
        len++;
    }
    s->b[s->cat - 2] = (uint8_t) ((len / 2) & 0xff);
    s->b[s->cat - 1] = (uint8_t) ((len / 2) >> 8);
}

static inline void img_end(sii_img_t *s)
{
    put16(s, 0xffff);
}

static inline void strings(sii_img_t *s, const char *const *str, unsigned int n)
{
    unsigned int i;

    cat_begin(s, 0x000A);
    put8(s, n);
    for (i = 0; i < n; i++) {
        size_t len = strlen(str[i]);

        put8(s, (unsigned int) len);
        memcpy(s->b + s->n, str[i], len);
        s->n += len;
    }
    cat_end(s);
}

static inline void sm(sii_img_t *s, unsigned int start, unsigned int size,
        unsigned int type)
{
    put16(s, start);
    put16(s, size);
    put8(s, 0x20);                   /* control */
    put8(s, 0);
    put8(s, 1);                      /* enable */
    put8(s, type);
}

static inline void pdo(sii_img_t *s, unsigned int index, unsigned int entries,
        unsigned int smi, unsigned int name)
{
    put16(s, index);
    put8(s, entries);
    put8(s, smi);
    put8(s, 0);
    put8(s, name);
    put16(s, 0);
}

static inline void entry(sii_img_t *s, unsigned int index, unsigned int sub,
        unsigned int name, unsigned int type, unsigned int bits)
{
    put16(s, index);
    put8(s, sub);
    put8(s, name);
    put8(s, type);
    put8(s, bits);
    put16(s, 0);
}

#endif /* ECSDO_SII_IMG_H */
