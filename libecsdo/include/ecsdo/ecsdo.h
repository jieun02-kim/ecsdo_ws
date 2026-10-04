/*
 * libecsdo v2 - read the CoE object dictionary (SDO list) of a slave and
 * upload single SDO entries through an IgH EtherCAT master, without
 * reserving the master. Read-only.
 *
 * This header exposes no IgH EtherCAT Master types.
 */

#ifndef ECSDO_ECSDO_H
#define ECSDO_ECSDO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECSDO_VERSION_MAJOR 2

/* Return codes: 0 = success, errors negative. */
#define ECSDO_OK                  0
#define ECSDO_ERR_INVALID_ARG    -1  /**< NULL pointer or bad argument. */
#define ECSDO_ERR_NO_MEMORY      -2
#define ECSDO_ERR_OPEN           -3  /**< Cannot open /dev/EtherCAT<n> (errno). */
#define ECSDO_ERR_VERSION        -4  /**< Kernel module ioctl version differs. */
#define ECSDO_ERR_IOCTL          -5  /**< ioctl failed: ecsdo_last_errno(). */
#define ECSDO_ERR_NO_SLAVE       -6  /**< No slave at this position. */
#define ECSDO_ERR_TIMEOUT        -7  /**< wait_ready: not all slaves ready. */
#define ECSDO_ERR_NOT_WRITABLE   -8  /**< Upload needs /dev/EtherCAT<n> O_RDWR. */
#define ECSDO_ERR_ABORT          -9  /**< Upload: slave answered with an SDO
                                          abort (see *abort_code). */
#define ECSDO_ERR_BUFFER_SIZE   -10  /**< Upload: value larger than buf. */
#define ECSDO_ERR_TYPE_MISMATCH -11  /**< Value size differs from its type's
                                          size (like `ethercat upload`). */
#define ECSDO_ERR_UNKNOWN_TYPE  -12  /**< Type not in the dictionary / not a
                                          known CoE type: raw bytes only. */

#define ECSDO_STRING_SIZE 64         /**< Fixed strings incl. NUL. */

/* AL states (ETG.1000.6) as found in ecsdo_slave_t::al_state. */
#define ECSDO_AL_INIT    0x01
#define ECSDO_AL_PREOP   0x02
#define ECSDO_AL_BOOT    0x03
#define ECSDO_AL_SAFEOP  0x04
#define ECSDO_AL_OP      0x08
#define ECSDO_AL_STATE_MASK 0x0F
#define ECSDO_AL_ERROR   0x10

/* Index into read_access / write_access. */
#define ECSDO_ACCESS_PREOP  0
#define ECSDO_ACCESS_SAFEOP 1
#define ECSDO_ACCESS_OP     2
#define ECSDO_ACCESS_COUNT  3

/* ------------------------------------------------------------------------
 * Dictionary (what `ethercat sdos` shows)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint8_t subindex;
    uint16_t data_type;              /**< CoE data type code (0x0007 =
                                          UNSIGNED32, ...). */
    uint16_t bit_length;
    uint8_t read_access[ECSDO_ACCESS_COUNT];
    uint8_t write_access[ECSDO_ACCESS_COUNT];
    char description[ECSDO_STRING_SIZE];
} ecsdo_entry_t;

typedef struct {
    uint16_t index;
    uint8_t max_subindex;
    char name[ECSDO_STRING_SIZE];
    size_t entry_count;              /**< Existing entries; gaps skipped. */
    ecsdo_entry_t *entries;          /**< Ascending subindex. */
} ecsdo_object_t;

typedef enum {
    ECSDO_SLAVE_OK = 0,              /**< Dictionary complete. */
    ECSDO_SLAVE_NO_COE,              /**< No CoE: no dictionary. */
    ECSDO_SLAVE_NO_SDO_INFO,         /**< CoE without SDO information:
                                          no dictionary, upload still works. */
    ECSDO_SLAVE_NOT_READY,           /**< Not scanned yet, or dictionary not
                                          confirmed complete; objects[] holds
                                          what the master had. */
    ECSDO_SLAVE_ERROR                /**< ioctl failed: sys_errno. */
} ecsdo_slave_status_t;

typedef struct {
    uint16_t position;
    uint16_t alias;
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
    uint32_t serial;
    char name[ECSDO_STRING_SIZE];
    uint8_t al_state;                /**< ECSDO_AL_*, maybe | ECSDO_AL_ERROR. */
    uint8_t coe;                     /**< CoE mailbox supported. */
    uint8_t sdo_info;                /**< SDO information supported. */
    ecsdo_slave_status_t status;
    int sys_errno;

    size_t object_count;
    ecsdo_object_t *objects;         /**< Dictionary order. */
} ecsdo_slave_t;

/* ------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

typedef struct ecsdo_ctx ecsdo_ctx_t;

/** Open /dev/EtherCAT<master_index> (O_RDWR, else O_RDONLY) and check the
 *  ioctl version. Does not reserve the master. */
int ecsdo_open(ecsdo_ctx_t **ctx, unsigned int master_index);
void ecsdo_close(ecsdo_ctx_t *ctx);

/** errno of the last failed ioctl on this handle. */
int ecsdo_last_errno(const ecsdo_ctx_t *ctx);

int ecsdo_slave_count(ecsdo_ctx_t *ctx, uint16_t *count);

/** Wait until the bus scan is done and every slave's dictionary is
 *  complete. The master fetches dictionaries some seconds after PREOP and
 *  has no "complete" flag; a slave counts as ready when it is in
 *  PREOP/SAFEOP/OP and the last object of its dictionary has entries,
 *  unchanged over three polls 100 ms apart. Returns ECSDO_ERR_TIMEOUT if not
 *  all slaves got ready; those are ECSDO_SLAVE_NOT_READY in later reads.
 *  Call it once before ecsdo_read_dict(). */
int ecsdo_wait_ready(ecsdo_ctx_t *ctx, unsigned int timeout_ms);

/** Read identity and dictionary (SDO list, no values) of one slave.
 *  Per-slave problems are reported in (*slave)->status with ECSDO_OK.
 *  Release with ecsdo_free_slave(). */
int ecsdo_read_dict(ecsdo_ctx_t *ctx, uint16_t position,
        ecsdo_slave_t **slave);
void ecsdo_free_slave(ecsdo_slave_t *slave);

/** Upload one SDO entry into buf, like `ethercat upload` / libethercat's
 *  ecrt_master_sdo_upload(). The raw value (little-endian) is stored in
 *  buf, its length in *data_size.
 *
 *  Returns ECSDO_OK, ECSDO_ERR_ABORT (*abort_code set, if not NULL),
 *  ECSDO_ERR_BUFFER_SIZE (retry with a larger buf), ECSDO_ERR_NO_SLAVE,
 *  ECSDO_ERR_NOT_WRITABLE or ECSDO_ERR_IOCTL. Blocks until the slave
 *  answered; not for use in a realtime loop. */
int ecsdo_upload(ecsdo_ctx_t *ctx, uint16_t position, uint16_t index,
        uint8_t subindex, void *buf, size_t buf_size, size_t *data_size,
        uint32_t *abort_code);

/* ------------------------------------------------------------------------
 * Typed upload and value decoding (what `ethercat upload` prints)
 * ---------------------------------------------------------------------- */

typedef enum {
    ECSDO_VALUE_RAW = 0,     /**< Not decoded: use data/size. */
    ECSDO_VALUE_UNSIGNED,    /**< BOOLEAN, BITn, UNSIGNEDn -> u */
    ECSDO_VALUE_SIGNED,      /**< INTEGERn -> i */
    ECSDO_VALUE_REAL,        /**< REAL32 / REAL64 -> f */
    ECSDO_VALUE_STRING       /**< VISIBLE_STRING -> str/str_len (trailing
                                  NUL padding removed, not NUL-terminated) */
} ecsdo_value_kind_t;

typedef struct {
    uint16_t data_type;      /**< CoE type code, 0 = unknown. */
    uint16_t bit_length;     /**< From the dictionary, 0 = unknown. */
    ecsdo_value_kind_t kind;
    uint64_t u;
    int64_t i;
    double f;
    const char *str;         /**< Points into data. */
    size_t str_len;
    const uint8_t *data;     /**< Raw little-endian bytes (caller's buf). */
    size_t size;
} ecsdo_value_t;

/** Like `ethercat upload`: look up the entry's type in the master's
 *  dictionary, upload, check the size against the type and decode.
 *  val->data points into buf; buf must outlive val.
 *
 *  Returns ECSDO_OK, or as ecsdo_upload(), or after a successful transfer:
 *  ECSDO_ERR_UNKNOWN_TYPE (entry not in the dictionary, e.g. no SDO info;
 *  decode yourself with ecsdo_decode()) or ECSDO_ERR_TYPE_MISMATCH. In those
 *  two cases val->data/size hold the raw value and val->kind is RAW. */
int ecsdo_upload_value(ecsdo_ctx_t *ctx, uint16_t position, uint16_t index,
        uint8_t subindex, void *buf, size_t buf_size, ecsdo_value_t *val,
        uint32_t *abort_code);

/** Decode raw bytes (e.g. from ecsdo_upload()) as data_type, e.g. taken
 *  from ecsdo_read_dict(). No I/O. Returns ECSDO_OK,
 *  ECSDO_ERR_UNKNOWN_TYPE (kind RAW; OCTET_STRING, DOMAIN, ... are RAW
 *  with ECSDO_OK) or ECSDO_ERR_TYPE_MISMATCH. */
int ecsdo_decode(uint16_t data_type, const void *data, size_t size,
        ecsdo_value_t *val);

/** CoE name of a data type ("UNSIGNED32"), or NULL if unknown. */
const char *ecsdo_type_name(uint16_t data_type);
/** Text of an SDO abort code (CiA 301 / ETG.1000.6), or NULL. */
const char *ecsdo_abort_string(uint32_t abort_code);

const char *ecsdo_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* ECSDO_ECSDO_H */
