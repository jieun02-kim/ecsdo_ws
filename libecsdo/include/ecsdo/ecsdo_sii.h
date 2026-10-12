/*
 * ecsdo_sii (part of libecsdo_util) - parse the SII categories the master
 * does not pass on: SyncM and TxPDO/RxPDO with entry data types and names.
 * Input is the raw SII from ecsdo_read_sii(); no I/O here.
 *
 * Layout (ETG.1000.6 / ETG.2010): categories start at word 0x0040, each
 * a type word and a size word (in words), the list ends with type 0xFFFF.
 *   Strings (10): count, then length-prefixed strings, 1-based index
 *   SyncM   (41): 8 bytes per SM: start, size, control, status, enable,
 *                 type
 *   TxPDO (50) / RxPDO (51): 8 bytes per PDO: index, entry count, SM,
 *                 DC sync, name string, flags; then 8 bytes per entry:
 *                 index, subindex, name string, data type, bit length,
 *                 flags
 */

#ifndef ECSDO_ECSDO_SII_H
#define ECSDO_ECSDO_SII_H

#include <stddef.h>
#include <stdint.h>

#include "ecsdo/ecsdo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SyncM type byte. */
#define ECSDO_SII_SM_UNUSED   0
#define ECSDO_SII_SM_MBX_OUT  1
#define ECSDO_SII_SM_MBX_IN   2
#define ECSDO_SII_SM_PD_OUT   3
#define ECSDO_SII_SM_PD_IN    4

typedef struct {
    uint16_t start_address;
    uint16_t default_size;
    uint8_t control;
    uint8_t enable;
    uint8_t type;                    /**< ECSDO_SII_SM_* */
} ecsdo_sii_sync_t;

typedef struct {
    uint16_t index;                  /**< 0 = gap */
    uint8_t subindex;
    uint8_t bit_length;
    uint16_t data_type;              /**< CoE type code, 0 if not given. */
    char name[ECSDO_STRING_SIZE];    /**< "" if no string. */
} ecsdo_sii_pdo_entry_t;

typedef enum {
    ECSDO_SII_RXPDO = 0,             /**< Outputs (master -> slave). */
    ECSDO_SII_TXPDO                  /**< Inputs (slave -> master). */
} ecsdo_sii_pdo_dir_t;

typedef struct {
    uint16_t index;
    ecsdo_sii_pdo_dir_t dir;
    int sync_index;                  /**< SM, -1 = not assigned by
                                          default (like the master). */
    char name[ECSDO_STRING_SIZE];
    size_t entry_count;
    ecsdo_sii_pdo_entry_t *entries;
} ecsdo_sii_pdo_t;

typedef struct {
    size_t sync_count;
    ecsdo_sii_sync_t *syncs;         /**< By SM index. */
    size_t pdo_count;
    ecsdo_sii_pdo_t *pdos;           /**< SII order. */
    uint8_t has_categories;          /**< Anything after word 0x0040. */
    uint8_t incomplete;              /**< Malformed SII: parsing stopped,
                                          what was read is kept (as the
                                          master does). */
} ecsdo_sii_t;

/** Parse raw SII bytes. Returns ECSDO_OK (check ->incomplete),
 *  ECSDO_ERR_INVALID_ARG or ECSDO_ERR_NO_MEMORY. Free with
 *  ecsdo_sii_free(). */
int ecsdo_sii_parse(const uint8_t *data, size_t size, ecsdo_sii_t **out);
void ecsdo_sii_free(ecsdo_sii_t *sii);

/** Process data in the SII: a PDO with entries, or a process data SM. */
int ecsdo_sii_has_process_data(const ecsdo_sii_t *sii);

#ifdef __cplusplus
}
#endif

#endif /* ECSDO_ECSDO_SII_H */
