/*
 * Fake ioctl layer; mirrors the IgH master behaviour the library relies on:
 * -EINVAL for missing slaves/objects/entries, an unfilled object reports
 * max_subindex 0, no name and no entries, -ENOBUFS when the target buffer
 * is too small, abort code copied back on failed uploads.
 */

#include "fake_ioctl.h"

#include <errno.h>
#include <string.h>

#include "ecsdo_ioctl.h"

#define FAKE_MAGIC 37u

fake_bus_t fake;

void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.magic = FAKE_MAGIC;
}

static void copy_name(char *dst, const char *src)
{
    dst[0] = '\0';
    if (src) {
        strncpy(dst, src, ECSDO_STRING_SIZE - 1);
        dst[ECSDO_STRING_SIZE - 1] = '\0';
    }
}

int ecsdo_io_open(unsigned int master_index, int read_write, int *fd)
{
    (void) master_index;
    if (fake.open_errno)
        return -fake.open_errno;
    if (read_write && fake.rw_open_errno)
        return -fake.rw_open_errno;
    fake.opened_rw = read_write;
    *fd = 1000;
    return 0;
}

void ecsdo_io_close(int fd)
{
    (void) fd;
}

uint32_t ecsdo_io_expected_magic(void)
{
    return FAKE_MAGIC;
}

int ecsdo_io_module(int fd, uint32_t *magic, uint32_t *master_count)
{
    (void) fd;
    *magic = fake.magic;
    *master_count = 1;
    return 0;
}

int ecsdo_io_master(int fd, ecsdo_io_master_t *out)
{
    (void) fd;
    if (fake.on_master_poll)
        fake.on_master_poll();
    memset(out, 0, sizeof(*out));
    out->slave_count = fake.slave_count;
    out->scan_busy = fake.scan_busy;
    return 0;
}

static fake_slave_t *slave_at(uint16_t position)
{
    return position < fake.slave_count ? &fake.slaves[position] : NULL;
}

int ecsdo_io_slave(int fd, uint16_t position, ecsdo_io_slave_t *out)
{
    fake_slave_t *s = slave_at(position);

    (void) fd;
    if (!s)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->vendor_id = s->vendor_id;
    out->product_code = s->product_code;
    out->revision = s->revision;
    out->serial = s->serial;
    out->alias = s->alias;
    out->mailbox_protocols = s->mailbox_protocols;
    out->has_general_category = s->has_general_category;
    out->enable_sdo_info = s->enable_sdo_info;
    out->al_state = s->al_state;
    out->sdo_count = (uint16_t) s->visible_objects;
    copy_name(out->name, s->name);
    return 0;
}

int ecsdo_io_sdo(int fd, uint16_t position, uint16_t sdo_position,
        ecsdo_io_sdo_t *out)
{
    fake_slave_t *s = slave_at(position);
    fake_object_t *o;

    (void) fd;
    if (!s || sdo_position >= s->visible_objects)
        return -EINVAL;
    o = &s->objects[sdo_position];
    if (o->sdo_errno)
        return -o->sdo_errno;

    memset(out, 0, sizeof(*out));
    out->index = o->index;
    if (sdo_position < s->filled_objects) {
        out->max_subindex = o->max_subindex;
        copy_name(out->name, o->name);
    }
    return 0;
}

int ecsdo_io_sdo_entry(int fd, uint16_t position, uint16_t sdo_position,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out)
{
    fake_slave_t *s = slave_at(position);
    fake_object_t *o;
    size_t i;

    (void) fd;
    if (!s || sdo_position >= s->visible_objects
            || sdo_position >= s->filled_objects)
        return -EINVAL;
    o = &s->objects[sdo_position];
    for (i = 0; i < o->entry_count; i++) {
        const fake_entry_t *e = &o->entries[i];

        if (e->subindex != subindex)
            continue;
        memset(out, 0, sizeof(*out));
        out->data_type = e->data_type;
        out->bit_length = e->bit_length;
        memcpy(out->read_access, e->read_access, 3);
        memcpy(out->write_access, e->write_access, 3);
        copy_name(out->description, e->description);
        return 0;
    }
    return -EINVAL;
}

int ecsdo_io_sdo_entry_by_index(int fd, uint16_t position, uint16_t index,
        uint8_t subindex, ecsdo_io_sdo_entry_t *out)
{
    fake_slave_t *s = slave_at(position);
    size_t i;

    if (!s || !index)
        return -EINVAL;
    for (i = 0; i < s->visible_objects; i++) {
        if (s->objects[i].index == index)
            return ecsdo_io_sdo_entry(fd, position, (uint16_t) i, subindex,
                    out);
    }
    return -EINVAL;
}

int ecsdo_io_sdo_upload(int fd, uint16_t position, uint16_t index,
        uint8_t subindex, uint8_t *target, size_t target_size,
        size_t *data_size, uint32_t *abort_code)
{
    fake_slave_t *s = slave_at(position);
    size_t i, j;

    (void) fd;
    if (fake.upload_calls < FAKE_MAX_UPLOADS)
        fake.upload_sizes[fake.upload_calls] = target_size;
    fake.upload_calls++;

    *data_size = 0;
    *abort_code = 0;
    if (!s)
        return -EINVAL;

    for (i = 0; i < s->object_count; i++) {
        fake_object_t *o = &s->objects[i];

        if (o->index != index)
            continue;
        for (j = 0; j < o->entry_count; j++) {
            fake_entry_t *e = &o->entries[j];

            if (e->subindex != subindex)
                continue;
            if (e->abort_code) {
                *abort_code = e->abort_code;
                return -EIO;
            }
            if (e->upload_errno)
                return -e->upload_errno;
            if (e->value_len > target_size)
                return -ENOBUFS;
            if (e->value_len)
                memcpy(target, e->value, e->value_len);
            *data_size = e->value_len;
            return 0;
        }
    }
    /* Not in the dictionary: the slave answers with an abort. */
    *abort_code = 0x06020000;
    return -EIO;
}
