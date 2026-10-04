/*
 * Fake implementation of src/ecsdo_ioctl.h for hardware-free tests.
 *
 * The test sets up `fake` (a simulated master with slaves), and can install
 * on_master_poll to advance the simulation each time the library queries
 * the master (scan progress, dictionary filling, ...).
 */

#ifndef ECSDO_FAKE_IOCTL_H
#define ECSDO_FAKE_IOCTL_H

#include <stddef.h>
#include <stdint.h>

#define FAKE_MAX_SLAVES 4
#define FAKE_MAX_OBJECTS 16
#define FAKE_MAX_ENTRIES 8
#define FAKE_MAX_UPLOADS 64

typedef struct {
    uint8_t subindex;
    uint16_t data_type;
    uint16_t bit_length;
    uint8_t read_access[3];
    uint8_t write_access[3];
    const char *description;
    const uint8_t *value;
    size_t value_len;
    uint32_t abort_code;      /* upload fails with SDO abort */
    int upload_errno;         /* upload fails with this errno */
} fake_entry_t;

typedef struct {
    uint16_t index;
    uint8_t max_subindex;
    const char *name;
    int sdo_errno;            /* SLAVE_SDO ioctl fails with this errno */
    size_t entry_count;
    fake_entry_t entries[FAKE_MAX_ENTRIES];
} fake_object_t;

typedef struct {
    uint32_t vendor_id, product_code, revision, serial;
    uint16_t alias;
    uint16_t mailbox_protocols;
    uint8_t has_general_category;
    uint8_t enable_sdo_info;
    uint8_t al_state;
    const char *name;

    size_t object_count;
    fake_object_t objects[FAKE_MAX_OBJECTS];
    size_t visible_objects;   /* reported sdo_count */
    size_t filled_objects;    /* objects with description and entries */
} fake_slave_t;

typedef struct {
    uint32_t magic;
    int rw_open_errno;        /* opening O_RDWR fails with this */
    int open_errno;           /* any open fails with this */
    uint8_t scan_busy;
    uint32_t slave_count;
    fake_slave_t slaves[FAKE_MAX_SLAVES];
    void (*on_master_poll)(void);

    /* recorded */
    int opened_rw;
    size_t upload_calls;
    size_t upload_sizes[FAKE_MAX_UPLOADS];
} fake_bus_t;

extern fake_bus_t fake;

/** Zero everything and set a valid ioctl magic. */
void fake_reset(void);

#endif /* ECSDO_FAKE_IOCTL_H */
