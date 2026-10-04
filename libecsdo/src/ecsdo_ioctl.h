/*
 * Thin wrappers around the IgH EtherCAT master character device ioctls.
 *
 * This header is IgH-free on purpose: ecsdo_ioctl.c is the only translation
 * unit that includes master/ioctl.h. All functions return 0 or -errno.
 */

#ifndef ECSDO_IOCTL_H
#define ECSDO_IOCTL_H

#include <stddef.h>
#include <stdint.h>

#include "ecsdo/ecsdo.h"

typedef struct {
    uint32_t slave_count;
    uint8_t scan_busy;
    uint8_t phase;
    uint8_t active;
} ecsdo_io_master_t;

typedef struct {
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
    uint32_t serial;
    uint16_t alias;
    uint16_t mailbox_protocols;
    uint8_t has_general_category;
    uint8_t enable_sdo_info;
    uint8_t al_state;
    uint8_t error_flag;
    uint16_t sdo_count;
    char name[ECSDO_STRING_SIZE];
    char order[ECSDO_STRING_SIZE];
    char group[ECSDO_STRING_SIZE];
} ecsdo_io_slave_t;

typedef struct {
    uint16_t index;
    uint8_t max_subindex;
    char name[ECSDO_STRING_SIZE];
} ecsdo_io_sdo_t;

typedef struct {
    uint16_t data_type;
    uint16_t bit_length;
    uint8_t read_access[ECSDO_ACCESS_COUNT];
    uint8_t write_access[ECSDO_ACCESS_COUNT];
    char description[ECSDO_STRING_SIZE];
} ecsdo_io_sdo_entry_t;

/** Mailbox protocol bit for CoE (ETG.1000.6). */
#define ECSDO_MBOX_COE 0x04

int ecsdo_io_open(unsigned int master_index, int read_write, int *fd);
void ecsdo_io_close(int fd);

uint32_t ecsdo_io_expected_magic(void);
int ecsdo_io_module(int fd, uint32_t *magic, uint32_t *master_count);
int ecsdo_io_master(int fd, ecsdo_io_master_t *out);
int ecsdo_io_slave(int fd, uint16_t position, ecsdo_io_slave_t *out);

/** Object at dictionary list position sdo_position. */
int ecsdo_io_sdo(int fd, uint16_t position, uint16_t sdo_position,
        ecsdo_io_sdo_t *out);

/** Entry of the object at dictionary list position sdo_position. */
int ecsdo_io_sdo_entry(int fd, uint16_t position, uint16_t sdo_position,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out);

/** Entry of the object with the given index (sdo_spec > 0 mode of the
 *  same ioctl). -EINVAL if not in the dictionary. */
int ecsdo_io_sdo_entry_by_index(int fd, uint16_t position, uint16_t index,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out);

/** Upload. -ENOBUFS means target_size was too small. On an SDO abort the
 *  return value is negative and *abort_code is non-zero. */
int ecsdo_io_sdo_upload(int fd, uint16_t position, uint16_t index,
        uint8_t subindex, uint8_t *target, size_t target_size,
        size_t *data_size, uint32_t *abort_code);

#endif /* ECSDO_IOCTL_H */
