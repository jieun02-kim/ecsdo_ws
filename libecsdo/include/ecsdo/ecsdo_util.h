/*
 * libecsdo_util - helpers on top of libecsdo (link both):
 *   - dump the dictionaries (SDO lists) of all slaves as JSON
 *   - read SDO values by name ("selector") into a table for control code
 *   - write such a table as JSON
 * Slave count/identity, PDO mapping, master control: use libethercat.
 *
 * The core library (ecsdo.h) does not depend on this module.
 */

#ifndef ECSDO_ECSDO_UTIL_H
#define ECSDO_ECSDO_UTIL_H

#include <stdio.h>

#include "ecsdo/ecsdo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Additional result codes for items. */
#define ECSDO_ERR_NOT_FOUND     -20  /**< Selector matches nothing. */
#define ECSDO_ERR_AMBIGUOUS     -21  /**< Selector matches several entries:
                                          add ":SUB" or "/description". */
#define ECSDO_ERR_NO_DICT       -22  /**< Slave has no (complete)
                                          dictionary; use "0xIDX:SUB". */
#define ECSDO_ERR_BAD_SELECTOR  -23  /**< Selector syntax error. */
#define ECSDO_ERR_WRITE         -24  /**< Writing the JSON output failed
                                          (errno set). */

/** Like ecsdo_strerror(), including the codes above. */
const char *ecsdo_util_strerror(int code);

/* ------------------------------------------------------------------------
 * Dictionary dump
 * ---------------------------------------------------------------------- */

/** Read the dictionary of every slave and write it as JSON to f
 *  (format "ecsdo-dict" version 1, see README). Call ecsdo_wait_ready()
 *  first. Slaves that cannot be read appear with their status.
 *  Returns ECSDO_OK, an error code, or ECSDO_ERR_WRITE. */
int ecsdo_dump_dict_json(ecsdo_ctx_t *ctx, unsigned int master_index,
        FILE *f);

/* ------------------------------------------------------------------------
 * Values by name
 * ---------------------------------------------------------------------- */

#define ECSDO_ITEM_BUF_SIZE 256       /**< Max value size per item. */

/** One wanted SDO value. Fill position and selector; the rest is output.
 *
 *  Selector forms (names compared ignoring ASCII case, spaces, '_', '-'):
 *    "Controlword"                   object name; the object must have a
 *                                    single entry (or only subindex 0)
 *    "Identity Object:1"             object name + subindex (dec or 0x)
 *    "Identity Object/Vendor ID"     object name + entry description
 *    "Vendor ID"                     entry description (if no object name
 *                                    matches)
 *    "0x1018:1", "0x1000"            index[:subindex], no dictionary needed
 *
 *  value points into buf: do not copy an item after ecsdo_get_items(). */
typedef struct {
    /* input */
    uint16_t position;
    const char *selector;

    /* output */
    int result;                       /**< ECSDO_OK or an error code
                                           (ecsdo_util_strerror()). */
    uint16_t index;
    uint8_t subindex;
    char object_name[ECSDO_STRING_SIZE];
    char description[ECSDO_STRING_SIZE];
    uint16_t data_type;
    uint16_t bit_length;
    uint8_t read_access[ECSDO_ACCESS_COUNT];
    uint8_t write_access[ECSDO_ACCESS_COUNT];
    ecsdo_value_t value;              /**< Decoded value (see ecsdo.h). */
    uint32_t abort_code;              /**< If result == ECSDO_ERR_ABORT. */
    uint8_t buf[ECSDO_ITEM_BUF_SIZE];
} ecsdo_item_t;

/** Resolve each item's selector in its slave's dictionary, upload the
 *  value and decode it with the dictionary's data type. Dictionaries are
 *  read once per slave. Call ecsdo_wait_ready() first.
 *
 *  Returns the number of items whose result is not ECSDO_OK (0 = all
 *  good), or a negative error code if nothing could be done. */
int ecsdo_get_items(ecsdo_ctx_t *ctx, ecsdo_item_t *items, size_t count);

/** Write items (after ecsdo_get_items) as JSON to f (format
 *  "ecsdo-items" version 1). Returns ECSDO_OK or ECSDO_ERR_WRITE. */
int ecsdo_items_json(const ecsdo_item_t *items, size_t count, FILE *f);

#ifdef __cplusplus
}
#endif

#endif /* ECSDO_ECSDO_UTIL_H */
