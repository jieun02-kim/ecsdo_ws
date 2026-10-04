/*
 * The only translation unit that includes IgH EtherCAT Master headers
 * (master/ioctl.h and master/globals.h, LGPL 2.1).
 *
 * Only read-only ioctls are used: MODULE, MASTER, SLAVE, SLAVE_SDO,
 * SLAVE_SDO_ENTRY and SLAVE_SDO_UPLOAD. Nothing here changes slave state
 * or reserves the master.
 */

#include "ecsdo_ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "master/ioctl.h"

/* The public header mirrors these values; fail the build if IgH differs. */
_Static_assert(EC_IOCTL_STRING_SIZE == ECSDO_STRING_SIZE,
        "string size mismatch");
_Static_assert((int) EC_SDO_ENTRY_ACCESS_COUNT == ECSDO_ACCESS_COUNT,
        "access state count mismatch");
_Static_assert((int) EC_SDO_ENTRY_ACCESS_PREOP == ECSDO_ACCESS_PREOP
        && (int) EC_SDO_ENTRY_ACCESS_SAFEOP == ECSDO_ACCESS_SAFEOP
        && (int) EC_SDO_ENTRY_ACCESS_OP == ECSDO_ACCESS_OP,
        "access state order mismatch");
_Static_assert(EC_SLAVE_STATE_INIT == ECSDO_AL_INIT
        && EC_SLAVE_STATE_PREOP == ECSDO_AL_PREOP
        && EC_SLAVE_STATE_BOOT == ECSDO_AL_BOOT
        && EC_SLAVE_STATE_SAFEOP == ECSDO_AL_SAFEOP
        && EC_SLAVE_STATE_OP == ECSDO_AL_OP
        && EC_SLAVE_STATE_ACK_ERR == ECSDO_AL_ERROR
        && EC_SLAVE_STATE_MASK == ECSDO_AL_STATE_MASK,
        "AL state values mismatch");
_Static_assert(EC_MBOX_COE == ECSDO_MBOX_COE, "CoE mailbox bit mismatch");

/** ioctl() that retries when interrupted; returns 0 or -errno. */
static int do_ioctl(int fd, unsigned long cmd, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, cmd, arg);
    } while (ret == -1 && errno == EINTR);

    return ret == -1 ? -errno : 0;
}

/** Copy an ioctl string field, always NUL-terminated. */
static void copy_str(char *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
    dst[size - 1] = '\0';
}

int ecsdo_io_open(unsigned int master_index, int read_write, int *fd)
{
    char path[32];
    int ret;

    snprintf(path, sizeof(path), "/dev/EtherCAT%u", master_index);

    do {
        ret = open(path, (read_write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1)
        return -errno;

    *fd = ret;
    return 0;
}

void ecsdo_io_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

uint32_t ecsdo_io_expected_magic(void)
{
    return EC_IOCTL_VERSION_MAGIC;
}

int ecsdo_io_module(int fd, uint32_t *magic, uint32_t *master_count)
{
    ec_ioctl_module_t data;
    int ret;

    memset(&data, 0, sizeof(data));
    ret = do_ioctl(fd, EC_IOCTL_MODULE, &data);
    if (ret)
        return ret;

    *magic = data.ioctl_version_magic;
    *master_count = data.master_count;
    return 0;
}

int ecsdo_io_master(int fd, ecsdo_io_master_t *out)
{
    ec_ioctl_master_t data;
    int ret;

    memset(&data, 0, sizeof(data));
    ret = do_ioctl(fd, EC_IOCTL_MASTER, &data);
    if (ret)
        return ret;

    out->slave_count = data.slave_count;
    out->scan_busy = data.scan_busy;
    out->phase = data.phase;
    out->active = data.active;
    return 0;
}

int ecsdo_io_slave(int fd, uint16_t position, ecsdo_io_slave_t *out)
{
    ec_ioctl_slave_t data;
    int ret;

    memset(&data, 0, sizeof(data));
    data.position = position;
    ret = do_ioctl(fd, EC_IOCTL_SLAVE, &data);
    if (ret)
        return ret;

    out->vendor_id = data.vendor_id;
    out->product_code = data.product_code;
    out->revision = data.revision_number;
    out->serial = data.serial_number;
    out->alias = data.alias;
    out->mailbox_protocols = data.mailbox_protocols;
    out->has_general_category = data.has_general_category;
    out->enable_sdo_info = data.coe_details.enable_sdo_info;
    out->al_state = data.al_state;
    out->error_flag = data.error_flag;
    out->sdo_count = data.sdo_count;
    copy_str(out->name, data.name, sizeof(out->name));
    copy_str(out->order, data.order, sizeof(out->order));
    copy_str(out->group, data.group, sizeof(out->group));
    return 0;
}

int ecsdo_io_sdo(int fd, uint16_t position, uint16_t sdo_position,
        ecsdo_io_sdo_t *out)
{
    ec_ioctl_slave_sdo_t data;
    int ret;

    memset(&data, 0, sizeof(data));
    data.slave_position = position;
    data.sdo_position = sdo_position;
    ret = do_ioctl(fd, EC_IOCTL_SLAVE_SDO, &data);
    if (ret)
        return ret;

    out->index = data.sdo_index;
    out->max_subindex = data.max_subindex;
    copy_str(out->name, data.name, sizeof(out->name));
    return 0;
}

/* sdo_spec: the master treats <= 0 as a negated list position, > 0 as an
 * object index. */
static int sdo_entry(int fd, uint16_t position, int sdo_spec,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out)
{
    ec_ioctl_slave_sdo_entry_t data;
    int i, ret;

    memset(&data, 0, sizeof(data));
    data.slave_position = position;
    data.sdo_spec = sdo_spec;
    data.sdo_entry_subindex = subindex;
    ret = do_ioctl(fd, EC_IOCTL_SLAVE_SDO_ENTRY, &data);
    if (ret)
        return ret;

    out->data_type = data.data_type;
    out->bit_length = data.bit_length;
    for (i = 0; i < ECSDO_ACCESS_COUNT; i++) {
        out->read_access[i] = data.read_access[i];
        out->write_access[i] = data.write_access[i];
    }
    copy_str(out->description, data.description, sizeof(out->description));
    return 0;
}

int ecsdo_io_sdo_entry(int fd, uint16_t position, uint16_t sdo_position,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out)
{
    return sdo_entry(fd, position, -(int) sdo_position, subindex, out);
}

int ecsdo_io_sdo_entry_by_index(int fd, uint16_t position, uint16_t index,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out)
{
    if (!index) /* 0 would mean list position 0 */
        return -EINVAL;
    return sdo_entry(fd, position, (int) index, subindex, out);
}

int ecsdo_io_sdo_upload(int fd, uint16_t position, uint16_t index,
        uint8_t subindex, uint8_t *target, size_t target_size,
        size_t *data_size, uint32_t *abort_code)
{
    ec_ioctl_slave_sdo_upload_t data;
    int ret;

    memset(&data, 0, sizeof(data));
    data.slave_position = position;
    data.sdo_index = index;
    data.sdo_entry_subindex = subindex;
    data.target_size = target_size;
    data.target = target;

    /* EINTR before the request is queued is safe to retry; after that the
     * master finishes the request before returning. */
    ret = do_ioctl(fd, EC_IOCTL_SLAVE_SDO_UPLOAD, &data);

    /* The master copies abort_code back even when the upload fails. */
    *abort_code = data.abort_code;
    *data_size = ret ? 0 : data.data_size;
    return ret;
}
