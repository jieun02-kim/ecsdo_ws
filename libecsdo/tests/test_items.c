/*
 * libecsdo_util tests: selectors, values by name, JSON output.
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "ecsdo/ecsdo_util.h"
#include "ecsdo_ioctl.h"
#include "fake_ioctl.h"
#include "test_util.h"

static const uint8_t v_devtype[] = {0x92, 0x01, 0x02, 0x00};
static const uint8_t v_four[] = {4};
static const uint8_t v_one[] = {1};
static const uint8_t v_vendor[] = {0x95, 0x75, 0x00, 0x00};
static const uint8_t v_product[] = {0x00, 0x00, 0x00, 0x00};
static const uint8_t v_1601[] = {0x01, 0x16};
static const uint8_t v_1a01[] = {0x01, 0x1a};
static const uint8_t v_cw[] = {0x06, 0x00};
static const uint8_t v_name[] = "L7NA004\0\0\0";
static const uint8_t v_two[] = {2};
static const uint8_t v_three[] = {3};
static const uint8_t v_m_cw[] = {0x10, 0x00, 0x40, 0x60};   /* 6040:00 16 */
static const uint8_t v_m_gap[] = {0x08, 0x00, 0x00, 0x00};  /* gap 8 */
static const uint8_t v_m_tp[] = {0x20, 0x00, 0x7a, 0x60};   /* 607A:00 32 */
static const uint8_t v_m_sw[] = {0x10, 0x00, 0x41, 0x60};   /* 6041:00 16 */
static const uint8_t v_m_pos[] = {0x20, 0x00, 0x64, 0x60};  /* 6064:00 32 */

static fake_entry_t ent(uint8_t sub, uint16_t type, uint16_t bits,
        const char *desc, const uint8_t *value, size_t len)
{
    fake_entry_t e;

    memset(&e, 0, sizeof(e));
    e.subindex = sub;
    e.data_type = type;
    e.bit_length = bits;
    e.read_access[0] = e.read_access[1] = e.read_access[2] = 1;
    e.description = desc;
    e.value = value;
    e.value_len = len;
    return e;
}

static void make_drive(fake_slave_t *s)
{
    fake_object_t *o;

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x7595;
    s->mailbox_protocols = ECSDO_MBOX_COE;
    s->has_general_category = 1;
    s->enable_sdo_info = 1;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "L7N \"drive\"\xff";
    s->object_count = 7;

    o = &s->objects[0];
    o->index = 0x1000; o->name = "Device Type"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0007, 32, "Device Type", v_devtype, 4);

    o = &s->objects[1];
    o->index = 0x1018; o->name = "Identity Object"; o->max_subindex = 4;
    o->entry_count = 3;
    o->entries[0] = ent(0, 0x0005, 8, "Number of entries", v_four, 1);
    o->entries[1] = ent(1, 0x0007, 32, "Vendor ID", v_vendor, 4);
    o->entries[2] = ent(2, 0x0007, 32, "Product code", v_product, 4);

    o = &s->objects[2];
    o->index = 0x1C12; o->name = "RxPDO(SM2) Assignment";
    o->max_subindex = 1; o->entry_count = 2;
    o->entries[0] = ent(0, 0x0005, 8, "", v_one, 1);
    o->entries[1] = ent(1, 0x0006, 16, "Index of object assigned to PDO",
            v_1601, 2);

    o = &s->objects[3];
    o->index = 0x1C13; o->name = "TxPDO(SM3) Assignment";
    o->max_subindex = 1; o->entry_count = 2;
    o->entries[0] = ent(0, 0x0005, 8, "", v_one, 1);
    o->entries[1] = ent(1, 0x0006, 16, "Index of object assigned to PDO",
            v_1a01, 2);

    o = &s->objects[4];
    o->index = 0x6040; o->name = "Controlword"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0006, 16, "Controlword", v_cw, 2);

    o = &s->objects[5];
    o->index = 0x1008; o->name = "Device Name"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0009, 88, "Device Name", v_name, 11);

    o = &s->objects[6];
    o->index = 0x1600; o->name = "1st Receive PDO Mapping";
    o->max_subindex = 10; o->entry_count = 1;
    o->entries[0] = ent(6, 0x0007, 32, "Mapping entry 6", NULL, 0);
    o->entries[0].abort_code = 0x06090011;

    o = &s->objects[7];
    o->index = 0x1601; o->name = "2nd Receive PDO Mapping";
    o->max_subindex = 3; o->entry_count = 4;
    o->entries[0] = ent(0, 0x0005, 8, "", v_three, 1);
    o->entries[1] = ent(1, 0x0007, 32, "Mapping entry 1", v_m_cw, 4);
    o->entries[2] = ent(2, 0x0007, 32, "Mapping entry 2", v_m_gap, 4);
    o->entries[3] = ent(3, 0x0007, 32, "Mapping entry 3", v_m_tp, 4);

    o = &s->objects[8];
    o->index = 0x1A01; o->name = "2nd Transmit PDO Mapping";
    o->max_subindex = 2; o->entry_count = 3;
    o->entries[0] = ent(0, 0x0005, 8, "", v_two, 1);
    o->entries[1] = ent(1, 0x0007, 32, "Mapping entry 1", v_m_sw, 4);
    o->entries[2] = ent(2, 0x0007, 32, "Mapping entry 2", v_m_pos, 4);

    o = &s->objects[9];
    o->index = 0x6041; o->name = "Statusword"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0006, 16, "Statusword", v_cw, 2);

    o = &s->objects[10];
    o->index = 0x607A; o->name = "Target Position"; o->entry_count = 1;
    o->entries[0] = ent(0, 0x0004, 32, "", v_vendor, 4); /* object name */

    s->object_count = 11;
    s->visible_objects = s->filled_objects = s->object_count;
}

static const ecsdo_item_t *find(const ecsdo_item_t *items, size_t n,
        const char *selector)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (!strcmp(items[i].selector, selector))
            return &items[i];
    }
    return NULL;
}

static void test_get_items(void)
{
    ecsdo_item_t items[] = {
        {.position = 0, .selector = "Device Type"},
        {.position = 0, .selector = "device_type"},
        {.position = 0, .selector = "Identity Object"},
        {.position = 0, .selector = "Identity Object:1"},
        {.position = 0, .selector = "Identity Object:0x02"},
        {.position = 0, .selector = "identity object / vendor id"},
        {.position = 0, .selector = "Vendor ID"},
        {.position = 0, .selector = "Index of object assigned to PDO"},
        {.position = 0, .selector = "RxPDO(SM2) Assignment:1"},
        {.position = 0, .selector = "TxPDO(SM3) Assignment/Index of object"
            " assigned to PDO"},
        {.position = 0, .selector = "0x1c13:1"},
        {.position = 0, .selector = "Control word"},
        {.position = 0, .selector = "Device Name"},
        {.position = 0, .selector = "1st Receive PDO Mapping:6"},
        {.position = 0, .selector = "Nope"},
        {.position = 0, .selector = "Identity Object:9"},
        {.position = 0, .selector = ""},
        {.position = 0, .selector = "0xZZ"},
        {.position = 0, .selector = "0x0:1"},
        {.position = 5, .selector = "Device Type"},
        {.position = 1, .selector = "Device Type"},
        {.position = 1, .selector = "0x1000"},
    };
    const size_t n = sizeof(items) / sizeof(items[0]);
    const ecsdo_item_t *it;
    ecsdo_ctx_t *ctx;
    int failed;

    fake_reset();
    fake.slave_count = 2;
    make_drive(&fake.slaves[0]);
    make_drive(&fake.slaves[1]);
    fake.slaves[1].enable_sdo_info = 0;          /* no dictionary */
    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);

    failed = ecsdo_get_items(ctx, items, n);

    it = find(items, n, "Device Type");
    CHECK_EQ(it->result, ECSDO_OK);
    CHECK_EQ(it->index, 0x1000);
    CHECK_EQ(it->subindex, 0);
    CHECK_STR(it->object_name, "Device Type");
    CHECK_EQ(it->data_type, 0x0007);
    CHECK_EQ(it->value.kind, ECSDO_VALUE_UNSIGNED);
    CHECK_EQ(it->value.u, 0x00020192);
    CHECK(it->value.data == it->buf);

    CHECK_EQ(find(items, n, "device_type")->value.u, 0x00020192);
    CHECK_EQ(find(items, n, "Identity Object")->result, ECSDO_ERR_AMBIGUOUS);

    it = find(items, n, "Identity Object:1");
    CHECK_EQ(it->result, ECSDO_OK);
    CHECK_EQ(it->value.u, 0x7595);
    CHECK_STR(it->description, "Vendor ID");
    CHECK_EQ(find(items, n, "Identity Object:0x02")->subindex, 2);
    CHECK_EQ(find(items, n, "identity object / vendor id")->value.u, 0x7595);

    it = find(items, n, "Vendor ID");                /* entry description */
    CHECK_EQ(it->result, ECSDO_OK);
    CHECK_EQ(it->index, 0x1018);
    CHECK_EQ(it->subindex, 1);

    CHECK_EQ(find(items, n, "Index of object assigned to PDO")->result,
            ECSDO_ERR_AMBIGUOUS);
    it = find(items, n, "RxPDO(SM2) Assignment:1");
    CHECK_EQ(it->result, ECSDO_OK);
    CHECK_EQ(it->value.u, 0x1601);
    CHECK_EQ(find(items, n, "TxPDO(SM3) Assignment/Index of object assigned"
                " to PDO")->value.u, 0x1a01);

    it = find(items, n, "0x1c13:1");                 /* by index */
    CHECK_EQ(it->result, ECSDO_OK);
    CHECK_EQ(it->data_type, 0x0006);                 /* type from dict */
    CHECK_EQ(it->value.u, 0x1a01);
    CHECK_STR(it->object_name, "TxPDO(SM3) Assignment");

    CHECK_EQ(find(items, n, "Control word")->value.u, 6);

    it = find(items, n, "Device Name");
    CHECK_EQ(it->value.kind, ECSDO_VALUE_STRING);
    CHECK_EQ(it->value.str_len, 7);

    it = find(items, n, "1st Receive PDO Mapping:6");
    CHECK_EQ(it->result, ECSDO_ERR_ABORT);
    CHECK_EQ(it->abort_code, 0x06090011);

    CHECK_EQ(find(items, n, "Nope")->result, ECSDO_ERR_NOT_FOUND);
    CHECK_EQ(find(items, n, "Identity Object:9")->result,
            ECSDO_ERR_NOT_FOUND);
    CHECK_EQ(find(items, n, "")->result, ECSDO_ERR_BAD_SELECTOR);
    CHECK_EQ(find(items, n, "0xZZ")->result, ECSDO_ERR_BAD_SELECTOR);
    CHECK_EQ(find(items, n, "0x0:1")->result, ECSDO_ERR_BAD_SELECTOR);

    CHECK_EQ(items[19].result, ECSDO_ERR_NO_SLAVE);  /* position 5 */
    CHECK_EQ(items[20].result, ECSDO_ERR_NO_DICT);   /* no SDO info: name */
    CHECK_EQ(items[21].result, ECSDO_ERR_UNKNOWN_TYPE); /* index: raw */
    CHECK_EQ(items[21].value.size, 4);
    CHECK(items[21].value.data == items[21].buf);

    CHECK_EQ(failed, 11); /* ambiguous 2, abort, not found 2, bad 3,
                             * no slave, no dict, unknown type */
    CHECK_EQ(ecsdo_get_items(ctx, NULL, 1), ECSDO_ERR_INVALID_ARG);
    CHECK_EQ(ecsdo_get_items(ctx, items, 0), 0);
    ecsdo_close(ctx);

    CHECK_STR(ecsdo_util_strerror(ECSDO_ERR_AMBIGUOUS),
            "selector matches several entries (add :SUB or /name)");
    CHECK_STR(ecsdo_util_strerror(ECSDO_ERR_ABORT), "SDO abort from slave");
}

static char *capture(int (*fn)(void *), void *arg, int *ret)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);

    *ret = fn ? 0 : -1;
    if (!f)
        return NULL;
    *ret = fn(f);
    fclose(f);
    (void) arg;
    return buf;
}

static ecsdo_ctx_t *g_ctx;
static ecsdo_item_t g_items[3];

static int do_dump(void *f)
{
    return ecsdo_dump_dict_json(g_ctx, 0, f);
}

static int do_items(void *f)
{
    return ecsdo_items_json(g_items, 3, f);
}

static void test_json(void)
{
    char *out;
    int ret;

    fake_reset();
    fake.slave_count = 2;
    make_drive(&fake.slaves[0]);
    memset(&fake.slaves[1], 0, sizeof(fake.slaves[1]));
    fake.slaves[1].al_state = ECSDO_AL_PREOP;        /* no CoE */
    fake.slaves[1].name = "EK1100";
    CHECK_EQ(ecsdo_open(&g_ctx, 0), ECSDO_OK);

    out = capture(do_dump, NULL, &ret);
    CHECK_EQ(ret, ECSDO_OK);
    CHECK(out != NULL);
    if (out) {
        size_t i;

        CHECK(strstr(out, "\"format\": \"ecsdo-dict\"") != NULL);
        CHECK(strstr(out, "\"index\": \"0x1018\", \"max_subindex\": 4, "
                    "\"name\": \"Identity Object\"") != NULL);
        CHECK(strstr(out, "\"type_name\": \"UNSIGNED32\"") != NULL);
        CHECK(strstr(out, "\"status\": \"NO_COE\"") != NULL);
        /* quote and 0xff byte escaped, output stays ASCII */
        CHECK(strstr(out, "\"L7N \\u0022drive\\u0022\\u00ff\"") != NULL);
        for (i = 0; out[i]; i++)
            CHECK((unsigned char) out[i] < 0x80);
        free(out);
    }
    CHECK_EQ(fake.upload_calls, 0);                  /* dictionary only */

    g_items[0].position = 0;
    g_items[0].selector = "Device Type";
    g_items[1].position = 0;
    g_items[1].selector = "Device Name";
    g_items[2].position = 0;
    g_items[2].selector = "Nope";
    ecsdo_get_items(g_ctx, g_items, 3);
    out = capture(do_items, NULL, &ret);
    CHECK_EQ(ret, ECSDO_OK);
    if (out) {
        CHECK(strstr(out, "\"format\": \"ecsdo-items\"") != NULL);
        CHECK(strstr(out, "\"value_hex\": \"92010200\", \"value\": 131474")
                != NULL);
        CHECK(strstr(out, "\"value\": \"L7NA004\"") != NULL);
        CHECK(strstr(out, "\"result\": -20, \"error\": \"no SDO entry"
                    " matches the selector\"") != NULL);
        free(out);
    }
    ecsdo_close(g_ctx);
}

int main(void)
{
    RUN(test_get_items);
    RUN(test_json);
    return TEST_RESULT();
}
