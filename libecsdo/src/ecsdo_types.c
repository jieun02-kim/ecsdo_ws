/*
 * CoE data types, value decoding and SDO abort texts.
 *
 * Sources:
 *  - CiA 301 7.4.7 (static data types 0x0001-0x001B), 7.2 (encoding:
 *    little-endian, two's complement, IEEE 754), 7.2.4.3.17 (abort codes).
 *  - ETG.1000.6 CoE: BIT1..BIT8 (0x0030-0x0037), abort codes 0x0601000x.
 * Written from the specifications.
 */

#include <string.h>

#include "ecsdo/ecsdo.h"

typedef struct {
    uint16_t code;
    const char *name;
    unsigned int bits;         /* fixed size, 0 = variable */
    ecsdo_value_kind_t kind;
} type_info_t;

static const type_info_t types[] = {
    {0x0001, "BOOLEAN", 1, ECSDO_VALUE_UNSIGNED},
    {0x0002, "INTEGER8", 8, ECSDO_VALUE_SIGNED},
    {0x0003, "INTEGER16", 16, ECSDO_VALUE_SIGNED},
    {0x0004, "INTEGER32", 32, ECSDO_VALUE_SIGNED},
    {0x0005, "UNSIGNED8", 8, ECSDO_VALUE_UNSIGNED},
    {0x0006, "UNSIGNED16", 16, ECSDO_VALUE_UNSIGNED},
    {0x0007, "UNSIGNED32", 32, ECSDO_VALUE_UNSIGNED},
    {0x0008, "REAL32", 32, ECSDO_VALUE_REAL},
    {0x0009, "VISIBLE_STRING", 0, ECSDO_VALUE_STRING},
    {0x000A, "OCTET_STRING", 0, ECSDO_VALUE_RAW},
    {0x000B, "UNICODE_STRING", 0, ECSDO_VALUE_RAW},
    {0x000C, "TIME_OF_DAY", 48, ECSDO_VALUE_RAW},
    {0x000D, "TIME_DIFFERENCE", 48, ECSDO_VALUE_RAW},
    {0x000F, "DOMAIN", 0, ECSDO_VALUE_RAW},
    {0x0010, "INTEGER24", 24, ECSDO_VALUE_SIGNED},
    {0x0011, "REAL64", 64, ECSDO_VALUE_REAL},
    {0x0012, "INTEGER40", 40, ECSDO_VALUE_SIGNED},
    {0x0013, "INTEGER48", 48, ECSDO_VALUE_SIGNED},
    {0x0014, "INTEGER56", 56, ECSDO_VALUE_SIGNED},
    {0x0015, "INTEGER64", 64, ECSDO_VALUE_SIGNED},
    {0x0016, "UNSIGNED24", 24, ECSDO_VALUE_UNSIGNED},
    {0x0018, "UNSIGNED40", 40, ECSDO_VALUE_UNSIGNED},
    {0x0019, "UNSIGNED48", 48, ECSDO_VALUE_UNSIGNED},
    {0x001A, "UNSIGNED56", 56, ECSDO_VALUE_UNSIGNED},
    {0x001B, "UNSIGNED64", 64, ECSDO_VALUE_UNSIGNED},
    {0x0030, "BIT1", 1, ECSDO_VALUE_UNSIGNED},
    {0x0031, "BIT2", 2, ECSDO_VALUE_UNSIGNED},
    {0x0032, "BIT3", 3, ECSDO_VALUE_UNSIGNED},
    {0x0033, "BIT4", 4, ECSDO_VALUE_UNSIGNED},
    {0x0034, "BIT5", 5, ECSDO_VALUE_UNSIGNED},
    {0x0035, "BIT6", 6, ECSDO_VALUE_UNSIGNED},
    {0x0036, "BIT7", 7, ECSDO_VALUE_UNSIGNED},
    {0x0037, "BIT8", 8, ECSDO_VALUE_UNSIGNED},
};

static const type_info_t *find_type(uint16_t code)
{
    size_t i;

    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (types[i].code == code)
            return &types[i];
    }
    return NULL;
}

const char *ecsdo_type_name(uint16_t data_type)
{
    const type_info_t *t = find_type(data_type);

    return t ? t->name : NULL;
}

int ecsdo_decode(uint16_t data_type, const void *data, size_t size,
        ecsdo_value_t *val)
{
    const type_info_t *t = find_type(data_type);
    const uint8_t *p = data;
    size_t nbytes, i;
    uint64_t v = 0;

    if (!val || (size && !data))
        return ECSDO_ERR_INVALID_ARG;
    memset(val, 0, sizeof(*val));
    val->data_type = data_type;
    val->data = p;
    val->size = size;
    val->kind = ECSDO_VALUE_RAW;
    if (!t)
        return ECSDO_ERR_UNKNOWN_TYPE;

    if (t->kind == ECSDO_VALUE_STRING) {
        /* VISIBLE_STRING is often NUL-padded to its declared length. */
        const void *nul = size ? memchr(p, '\0', size) : NULL;

        val->kind = ECSDO_VALUE_STRING;
        val->str = (const char *) p;
        val->str_len = nul ? (size_t) ((const uint8_t *) nul - p) : size;
        return ECSDO_OK;
    }
    if (!t->bits || t->kind == ECSDO_VALUE_RAW)
        return ECSDO_OK; /* variable-length or not decoded: raw */

    /* Type check: the value must have exactly the type's size. */
    nbytes = (t->bits + 7u) / 8u;
    if (size != nbytes)
        return ECSDO_ERR_TYPE_MISMATCH;

    for (i = 0; i < nbytes; i++)
        v |= (uint64_t) p[i] << (8u * i);
    if (t->bits < 64)
        v &= ((uint64_t) 1 << t->bits) - 1u;

    switch (t->kind) {
        case ECSDO_VALUE_UNSIGNED:
            val->u = v;
            break;
        case ECSDO_VALUE_SIGNED:
            if (t->bits < 64) {
                uint64_t sign = (uint64_t) 1 << (t->bits - 1u);
                v = (v ^ sign) - sign; /* sign-extend */
            }
            val->i = (int64_t) v;
            break;
        case ECSDO_VALUE_REAL:
            if (t->bits == 32) {
                uint32_t u32 = (uint32_t) v;
                float f;
                memcpy(&f, &u32, sizeof(f));
                val->f = f;
            } else {
                memcpy(&val->f, &v, sizeof(val->f));
            }
            break;
        default:
            break;
    }
    val->kind = t->kind;
    return ECSDO_OK;
}

/* ------------------------------------------------------------------------ */

static const struct {
    uint32_t code;
    const char *text;
} aborts[] = {
    {0x05030000, "Toggle bit not alternated"},
    {0x05040000, "SDO protocol timed out"},
    {0x05040001, "Client/server command specifier not valid or unknown"},
    {0x05040002, "Invalid block size"},
    {0x05040003, "Invalid sequence number"},
    {0x05040004, "CRC error"},
    {0x05040005, "Out of memory"},
    {0x06010000, "Unsupported access to an object"},
    {0x06010001, "Attempt to read a write-only object"},
    {0x06010002, "Attempt to write a read-only object"},
    {0x06010003, "Subindex cannot be written, subindex 0 must be 0 for"
        " write access"},
    {0x06010004, "SDO complete access not supported"},
    {0x06010005, "Object length exceeds mailbox size"},
    {0x06010006, "Object mapped to RxPDO, SDO download blocked"},
    {0x06020000, "Object does not exist in the object dictionary"},
    {0x06040041, "Object cannot be mapped to the PDO"},
    {0x06040042, "Number and length of mapped objects would exceed PDO"
        " length"},
    {0x06040043, "General parameter incompatibility"},
    {0x06040047, "General internal incompatibility in the device"},
    {0x06060000, "Access failed due to a hardware error"},
    {0x06070010, "Data type does not match, length of service parameter"
        " does not match"},
    {0x06070012, "Data type does not match, length of service parameter"
        " too high"},
    {0x06070013, "Data type does not match, length of service parameter"
        " too low"},
    {0x06090011, "Subindex does not exist"},
    {0x06090030, "Value range of parameter exceeded"},
    {0x06090031, "Value of parameter written too high"},
    {0x06090032, "Value of parameter written too low"},
    {0x06090036, "Maximum value is less than minimum value"},
    {0x060A0023, "Resource not available"},
    {0x08000000, "General error"},
    {0x08000020, "Data cannot be transferred or stored to the application"},
    {0x08000021, "Data cannot be transferred or stored to the application"
        " because of local control"},
    {0x08000022, "Data cannot be transferred or stored to the application"
        " because of the present device state"},
    {0x08000023, "Object dictionary dynamic generation failed or no object"
        " dictionary is present"},
    {0x08000024, "No data available"},
};

const char *ecsdo_abort_string(uint32_t abort_code)
{
    size_t i;

    for (i = 0; i < sizeof(aborts) / sizeof(aborts[0]); i++) {
        if (aborts[i].code == abort_code)
            return aborts[i].text;
    }
    return NULL;
}
