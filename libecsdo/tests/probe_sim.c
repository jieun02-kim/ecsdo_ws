/*
 * device_probe against a simulated bus (fake ioctl, no hardware):
 *   0: CiA 402 servo drive (CoE, SDO info, PDO mapping, missing 0x100A)
 *   1: digital input terminal (no mailbox, SII PDOs only)
 *   2: coupler (no process data)
 *   3: CoE device with an MDP device type (two digital inputs, assigned
 *      PDOs readable)
 *   4: the drive without SDO information and without 0x1C13 (TxPDOs
 *      fall back to the SII)
 *   5: force/torque sensor without a device profile (other_profile:
 *      PDOs, dictionary and all SII PDOs dumped)
 * All identities and names are made up.
 *
 *   probe_sim OUT_DIR VENDOR_IDS_JSON
 *
 * Checks the branch of each JSON; the files are kept for inspection.
 */

#include <stdlib.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo/ecsdo_sii.h"
#include "fake_ioctl.h"
#include "sii_img.h"
#include "test_util.h"

int device_probe_main(int argc, char **argv);

/* Little-endian values. */
static const uint8_t v_devtype[] = {0x92, 0x01, 0x02, 0x00};    /* 402 */
static const uint8_t v_devtype_mdp[] = {0x89, 0x13, 0x00, 0x00}; /* 5001 */
static const uint8_t v_name[] = "SimServo";
static const uint8_t v_hw[] = "HW1.0";
static const uint8_t v_u8_1[] = {1};
static const uint8_t v_u8_2[] = {2};
static const uint8_t v_u8_3[] = {3};
static const uint8_t v_u8_4[] = {4};
static const uint8_t v_vendor[] = {0x34, 0x12, 0x00, 0x00};
static const uint8_t v_product[] = {0x01, 0x00, 0x00, 0x00};
static const uint8_t v_rev[] = {0x02, 0x00, 0x00, 0x00};
static const uint8_t v_serial[] = {0x55, 0x00, 0x00, 0x00};
static const uint8_t v_1600[] = {0x00, 0x16};
static const uint8_t v_1a00[] = {0x00, 0x1a};
static const uint8_t v_map_6040[] = {0x10, 0x00, 0x40, 0x60};
static const uint8_t v_map_607a[] = {0x20, 0x00, 0x7a, 0x60};
static const uint8_t v_map_6041[] = {0x10, 0x00, 0x41, 0x60};
static const uint8_t v_map_6064[] = {0x20, 0x00, 0x64, 0x60};
static const uint8_t v_map_gap[] = {0x08, 0x00, 0x00, 0x00};
static const uint8_t v_u8_0[] = {0};
static const uint8_t v_1a01[] = {0x01, 0x1a};
static const uint8_t v_map_6000_1[] = {0x01, 0x01, 0x00, 0x60};
static const uint8_t v_map_6010_1[] = {0x01, 0x01, 0x10, 0x60};
static const uint8_t v_devtype_none[] = {0x00, 0x00, 0x00, 0x00};
static const uint8_t v_name_ft[] = "SimFT6";
static const uint8_t v_vendor_ft[] = {0x78, 0x56, 0x00, 0x00};
static const uint8_t v_product_ft[] = {0x06, 0x00, 0x00, 0x00};
static const uint8_t v_rev_ft[] = {0x01, 0x00, 0x00, 0x00};
static const uint8_t v_map_6000_1_32[] = {0x20, 0x01, 0x00, 0x60};
static const uint8_t v_map_6000_2_32[] = {0x20, 0x02, 0x00, 0x60};
static const uint8_t v_map_6000_3_32[] = {0x20, 0x03, 0x00, 0x60};
static const uint8_t v_map_6010_1_32[] = {0x20, 0x01, 0x10, 0x60};
static const uint8_t v_map_7000_1_16[] = {0x10, 0x01, 0x00, 0x70};
static const uint8_t v_modes[] = {0xa5, 0x03, 0x00, 0x00};  /* PP PV HM
                                                               CSP CSV
                                                               CST */

static fake_entry_t ent(uint8_t sub, uint16_t type, uint16_t bits, int rw,
        const char *desc, const uint8_t *value, size_t len)
{
    fake_entry_t e;

    memset(&e, 0, sizeof(e));
    e.subindex = sub;
    e.data_type = type;
    e.bit_length = bits;
    e.read_access[0] = e.read_access[1] = e.read_access[2] = 1;
    e.write_access[0] = e.write_access[1] = e.write_access[2] =
        (uint8_t) rw;
    e.description = desc;
    e.value = value;
    e.value_len = len;
    return e;
}

static fake_object_t *obj(fake_slave_t *s, uint16_t index,
        const char *name, uint8_t max_sub)
{
    fake_object_t *o = &s->objects[s->object_count++];

    o->index = index;
    o->name = name;
    o->max_subindex = max_sub;
    return o;
}

static void add(fake_object_t *o, fake_entry_t e)
{
    o->entries[o->entry_count++] = e;
}

static sii_img_t sii_drive, sii_term, sii_coupler, sii_mdp, sii_ft;

static void make_drive(fake_slave_t *s)
{
    static const char *const str[] = {
        "SimServo", "Controlword", "Target position", "Statusword",
        "Position actual value",
    };
    fake_object_t *o;

    img_start(&sii_drive);
    strings(&sii_drive, str, 5);
    cat_begin(&sii_drive, 0x0029);
    sm(&sii_drive, 0x1000, 128, ECSDO_SII_SM_MBX_OUT);
    sm(&sii_drive, 0x1080, 128, ECSDO_SII_SM_MBX_IN);
    sm(&sii_drive, 0x1100, 0, ECSDO_SII_SM_PD_OUT);
    sm(&sii_drive, 0x1180, 0, ECSDO_SII_SM_PD_IN);
    cat_end(&sii_drive);
    cat_begin(&sii_drive, 0x0033);
    pdo(&sii_drive, 0x1600, 2, 2, 0);
    entry(&sii_drive, 0x6040, 0, 2, 0x0006, 16);
    entry(&sii_drive, 0x607A, 0, 3, 0x0004, 32);
    cat_end(&sii_drive);
    cat_begin(&sii_drive, 0x0032);
    pdo(&sii_drive, 0x1A00, 2, 3, 0);
    entry(&sii_drive, 0x6041, 0, 4, 0x0006, 16);
    entry(&sii_drive, 0x6064, 0, 5, 0x0004, 32);
    cat_end(&sii_drive);
    img_end(&sii_drive);

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x1234;
    s->product_code = 1;
    s->revision = 2;
    s->mailbox_protocols = ECSDO_MBOX_COE | ECSDO_MBOX_FOE;
    s->has_general_category = 1;
    s->enable_sdo_info = 1;
    s->enable_pdo_assign = 1;
    s->enable_pdo_configuration = 1;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "SimServo";
    s->order = "SIM-SERVO-100";
    s->group = "Servo Drives";
    s->sii = sii_drive.b;
    s->sii_size = sii_drive.n;

    o = obj(s, 0x1000, "Device Type", 0);
    add(o, ent(0, 0x0007, 32, 0, "", v_devtype, 4));
    o = obj(s, 0x1008, "Device Name", 0);
    add(o, ent(0, 0x0009, 64, 0, "", v_name, 8));
    o = obj(s, 0x1009, "Hardware Version", 0);
    add(o, ent(0, 0x0009, 40, 0, "", v_hw, 5));
    /* 0x100A missing: upload aborts. */
    o = obj(s, 0x1018, "Identity Object", 4);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_4, 1));
    add(o, ent(1, 0x0007, 32, 0, "Vendor ID", v_vendor, 4));
    add(o, ent(2, 0x0007, 32, 0, "Product Code", v_product, 4));
    add(o, ent(3, 0x0007, 32, 0, "Revision Number", v_rev, 4));
    add(o, ent(4, 0x0007, 32, 0, "Serial Number", v_serial, 4));
    o = obj(s, 0x1600, "1st RxPDO Mapping", 2);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_2, 1));
    add(o, ent(1, 0x0007, 32, 1, "", v_map_6040, 4));
    add(o, ent(2, 0x0007, 32, 1, "", v_map_607a, 4));
    o = obj(s, 0x1A00, "1st TxPDO Mapping", 3);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_3, 1));
    add(o, ent(1, 0x0007, 32, 1, "", v_map_6041, 4));
    add(o, ent(2, 0x0007, 32, 1, "", v_map_6064, 4));
    add(o, ent(3, 0x0007, 32, 1, "", v_map_gap, 4));
    o = obj(s, 0x1C12, "RxPDO Assign", 1);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_1, 1));
    add(o, ent(1, 0x0006, 16, 1, "", v_1600, 2));
    o = obj(s, 0x1C13, "TxPDO Assign", 1);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_1, 1));
    add(o, ent(1, 0x0006, 16, 1, "", v_1a00, 2));
    o = obj(s, 0x2000, "Motor ID", 0);
    add(o, ent(0, 0x0006, 16, 1, "", NULL, 0));
    o = obj(s, 0x2100, "Inertia Ratio", 0);
    add(o, ent(0, 0x0006, 16, 1, "", NULL, 0));
    o = obj(s, 0x6040, "Controlword", 0);
    add(o, ent(0, 0x0006, 16, 1, "", NULL, 0));
    o = obj(s, 0x6041, "Statusword", 0);
    add(o, ent(0, 0x0006, 16, 0, "", NULL, 0));
    o = obj(s, 0x6064, "Position actual value", 0);
    add(o, ent(0, 0x0004, 32, 0, "", NULL, 0));
    o = obj(s, 0x607A, "Target position", 0);
    add(o, ent(0, 0x0004, 32, 1, "", NULL, 0));
    o = obj(s, 0x607D, "Software position limit", 2);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_2, 1));
    add(o, ent(1, 0x0004, 32, 1, "Min position limit", NULL, 0));
    add(o, ent(2, 0x0004, 32, 1, "Max position limit", NULL, 0));
    o = obj(s, 0x6502, "Supported drive modes", 0);
    add(o, ent(0, 0x0007, 32, 0, "", v_modes, 4));
    o = obj(s, 0x6860, "Modes of operation (axis 1)", 0);
    add(o, ent(0, 0x0002, 8, 1, "", NULL, 0));
    /* Write only in PREOP: shows the per-state access form. */
    o->entries[0].write_access[1] = o->entries[0].write_access[2] = 0;

    s->visible_objects = s->filled_objects = s->object_count;
}

static void make_terminal(fake_slave_t *s)
{
    static const char *const str[] = {
        "SimIn2", "Channel 1", "Channel 2", "Input", "DigIn",
    };

    img_start(&sii_term);
    strings(&sii_term, str, 5);
    cat_begin(&sii_term, 0x0029);
    sm(&sii_term, 0x1000, 1, ECSDO_SII_SM_PD_IN);
    cat_end(&sii_term);
    cat_begin(&sii_term, 0x0032);
    pdo(&sii_term, 0x1A00, 1, 0, 2);
    entry(&sii_term, 0x6000, 1, 4, 0x0001, 1);
    pdo(&sii_term, 0x1A01, 1, 0, 3);
    entry(&sii_term, 0x6010, 1, 4, 0x0001, 1);
    pdo(&sii_term, 0x1A02, 1, 0xFF, 0);      /* not assigned */
    entry(&sii_term, 0x6020, 1, 4, 0x0001, 1);
    cat_end(&sii_term);
    img_end(&sii_term);

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x2;
    s->product_code = 0x0000abcd;
    s->revision = 0x00100000;
    s->has_general_category = 1;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "SimIn2";
    s->order = "SIM-IN2";
    s->group = "DigIn";
    s->sii = sii_term.b;
    s->sii_size = sii_term.n;
}

static void make_coupler(fake_slave_t *s)
{
    static const char *const str[] = {"SimCoupler"};

    img_start(&sii_coupler);
    strings(&sii_coupler, str, 1);
    img_end(&sii_coupler);

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x2;
    s->product_code = 0x0000abce;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "SimCoupler";
    s->sii = sii_coupler.b;
    s->sii_size = sii_coupler.n;
}

/** CoE, no General category (SDO info "unknown"), MDP device type. */
static void make_mdp(fake_slave_t *s)
{
    static const char *const str[] = {
        "SimMDP", "Channel 1", "Channel 2", "Input",
    };
    fake_object_t *o;

    img_start(&sii_mdp);
    strings(&sii_mdp, str, 4);
    cat_begin(&sii_mdp, 0x0029);
    sm(&sii_mdp, 0x1000, 128, ECSDO_SII_SM_MBX_OUT);
    sm(&sii_mdp, 0x1080, 128, ECSDO_SII_SM_MBX_IN);
    sm(&sii_mdp, 0x1100, 0, ECSDO_SII_SM_PD_OUT);
    sm(&sii_mdp, 0x1180, 0, ECSDO_SII_SM_PD_IN);
    cat_end(&sii_mdp);
    cat_begin(&sii_mdp, 0x0032);
    pdo(&sii_mdp, 0x1A00, 1, 3, 2);
    entry(&sii_mdp, 0x6000, 1, 4, 0x0001, 1);
    pdo(&sii_mdp, 0x1A01, 1, 3, 3);
    entry(&sii_mdp, 0x6010, 1, 4, 0x0001, 1);
    cat_end(&sii_mdp);
    img_end(&sii_mdp);

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x2;
    s->product_code = 0x0000abcf;
    s->mailbox_protocols = ECSDO_MBOX_COE;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "SimMDP";
    s->sii = sii_mdp.b;
    s->sii_size = sii_mdp.n;
    o = obj(s, 0x1000, "Device Type", 0);
    add(o, ent(0, 0x0007, 32, 0, "", v_devtype_mdp, 4));
    o = obj(s, 0x1A00, "Channel 1", 1);
    add(o, ent(0, 0x0005, 8, 0, "", v_u8_1, 1));
    add(o, ent(1, 0x0007, 32, 0, "", v_map_6000_1, 4));
    o = obj(s, 0x1A01, "Channel 2", 1);
    add(o, ent(0, 0x0005, 8, 0, "", v_u8_1, 1));
    add(o, ent(1, 0x0007, 32, 0, "", v_map_6010_1, 4));
    o = obj(s, 0x1C12, "RxPDO Assign", 0);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_0, 1));
    o = obj(s, 0x1C13, "TxPDO Assign", 2);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_2, 1));
    add(o, ent(1, 0x0006, 16, 1, "", v_1a00, 2));
    add(o, ent(2, 0x0006, 16, 1, "", v_1a01, 2));
    o = obj(s, 0x6000, "Inputs Ch.1", 1);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_1, 1));
    add(o, ent(1, 0x0001, 1, 0, "Input", NULL, 0));
    o = obj(s, 0x6010, "Inputs Ch.2", 1);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_1, 1));
    add(o, ent(1, 0x0001, 1, 0, "Input", NULL, 0));
    s->visible_objects = s->filled_objects = s->object_count;
}

static void make_drive_no_info(fake_slave_t *s)
{
    size_t i;

    make_drive(s);
    s->enable_sdo_info = 0;
    for (i = 0; i < s->object_count; i++)
        if (s->objects[i].index == 0x1C13)
            s->objects[i].index = 0x1C99; /* upload aborts */
}

/** CoE with SDO info, device type 0 (no profile). TxPDO 0x1A00 (forces)
 *  assigned, 0x1A01 (status) only in the SII and not assigned. */
static void make_ft_sensor(fake_slave_t *s)
{
    static const char *const str[] = {
        "SimFT6", "Forces", "Status", "Control", "Force X", "Force Y",
        "Force Z", "Sensor status", "Command",
    };
    fake_object_t *o;

    img_start(&sii_ft);
    strings(&sii_ft, str, 9);
    cat_begin(&sii_ft, 0x0029);
    sm(&sii_ft, 0x1000, 128, ECSDO_SII_SM_MBX_OUT);
    sm(&sii_ft, 0x1080, 128, ECSDO_SII_SM_MBX_IN);
    sm(&sii_ft, 0x1100, 0, ECSDO_SII_SM_PD_OUT);
    sm(&sii_ft, 0x1180, 0, ECSDO_SII_SM_PD_IN);
    cat_end(&sii_ft);
    cat_begin(&sii_ft, 0x0033);
    pdo(&sii_ft, 0x1600, 1, 2, 4);
    entry(&sii_ft, 0x7000, 1, 9, 0x0006, 16);
    cat_end(&sii_ft);
    cat_begin(&sii_ft, 0x0032);
    pdo(&sii_ft, 0x1A00, 3, 3, 2);
    entry(&sii_ft, 0x6000, 1, 5, 0x0004, 32);
    entry(&sii_ft, 0x6000, 2, 6, 0x0004, 32);
    entry(&sii_ft, 0x6000, 3, 7, 0x0004, 32);
    pdo(&sii_ft, 0x1A01, 1, 0xFF, 3);      /* not assigned */
    entry(&sii_ft, 0x6010, 1, 8, 0x0007, 32);
    cat_end(&sii_ft);
    img_end(&sii_ft);

    memset(s, 0, sizeof(*s));
    s->vendor_id = 0x5678;
    s->product_code = 6;
    s->revision = 1;
    s->mailbox_protocols = ECSDO_MBOX_COE;
    s->has_general_category = 1;
    s->enable_sdo_info = 1;
    s->enable_pdo_assign = 1;
    s->al_state = ECSDO_AL_PREOP;
    s->name = "SimFT6";
    s->order = "SIM-FT6";
    s->group = "Sensors";
    s->sii = sii_ft.b;
    s->sii_size = sii_ft.n;

    o = obj(s, 0x1000, "Device Type", 0);
    add(o, ent(0, 0x0007, 32, 0, "", v_devtype_none, 4));
    o = obj(s, 0x1008, "Device Name", 0);
    add(o, ent(0, 0x0009, 48, 0, "", v_name_ft, 6));
    o = obj(s, 0x1018, "Identity Object", 3);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_3, 1));
    add(o, ent(1, 0x0007, 32, 0, "Vendor ID", v_vendor_ft, 4));
    add(o, ent(2, 0x0007, 32, 0, "Product Code", v_product_ft, 4));
    add(o, ent(3, 0x0007, 32, 0, "Revision Number", v_rev_ft, 4));
    o = obj(s, 0x1600, "Control", 1);
    add(o, ent(0, 0x0005, 8, 0, "", v_u8_1, 1));
    add(o, ent(1, 0x0007, 32, 0, "", v_map_7000_1_16, 4));
    o = obj(s, 0x1A00, "Forces", 3);
    add(o, ent(0, 0x0005, 8, 0, "", v_u8_3, 1));
    add(o, ent(1, 0x0007, 32, 0, "", v_map_6000_1_32, 4));
    add(o, ent(2, 0x0007, 32, 0, "", v_map_6000_2_32, 4));
    add(o, ent(3, 0x0007, 32, 0, "", v_map_6000_3_32, 4));
    o = obj(s, 0x1A01, "Status", 1);
    add(o, ent(0, 0x0005, 8, 0, "", v_u8_1, 1));
    add(o, ent(1, 0x0007, 32, 0, "", v_map_6010_1_32, 4));
    o = obj(s, 0x1C12, "RxPDO Assign", 1);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_1, 1));
    add(o, ent(1, 0x0006, 16, 1, "", v_1600, 2));
    o = obj(s, 0x1C13, "TxPDO Assign", 1);
    add(o, ent(0, 0x0005, 8, 1, "", v_u8_1, 1));
    add(o, ent(1, 0x0006, 16, 1, "", v_1a00, 2));
    o = obj(s, 0x2000, "Filter frequency", 0);
    add(o, ent(0, 0x0006, 16, 1, "", NULL, 0));
    o = obj(s, 0x6000, "Force", 3);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_3, 1));
    add(o, ent(1, 0x0004, 32, 0, "Force X", NULL, 0));
    add(o, ent(2, 0x0004, 32, 0, "Force Y", NULL, 0));
    add(o, ent(3, 0x0004, 32, 0, "Force Z", NULL, 0));
    o = obj(s, 0x6010, "Status", 1);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_1, 1));
    add(o, ent(1, 0x0007, 32, 0, "Sensor status", NULL, 0));
    o = obj(s, 0x7000, "Control", 1);
    add(o, ent(0, 0x0005, 8, 0, "Number of entries", v_u8_1, 1));
    add(o, ent(1, 0x0006, 16, 1, "Command", NULL, 0));
    s->visible_objects = s->filled_objects = s->object_count;
}

static int contains(const char *dir, int pos, const char *what)
{
    char path[512], text[16384];
    size_t n;
    FILE *f;

    snprintf(path, sizeof(path), "%s/slave_%d.json", dir, pos);
    f = fopen(path, "r");
    if (!f)
        return 0;
    n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    return strstr(text, what) != NULL;
}

static void check_branch(const char *dir, int pos, const char *branch)
{
    char path[512], text[16384], want[64];
    size_t n;
    FILE *f;

    snprintf(path, sizeof(path), "%s/slave_%d.json", dir, pos);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    snprintf(want, sizeof(want), "\"branch\": \"%s\"", branch);
    CHECK(strstr(text, want) != NULL);
}

int main(int argc, char **argv)
{
    char *args[8];

    if (argc != 3) {
        fprintf(stderr, "usage: %s OUT_DIR VENDOR_IDS_JSON\n", argv[0]);
        return 2;
    }
    fake_reset();
    fake.slave_count = 6;
    make_drive(&fake.slaves[0]);
    make_terminal(&fake.slaves[1]);
    make_coupler(&fake.slaves[2]);
    make_mdp(&fake.slaves[3]);
    make_drive_no_info(&fake.slaves[4]);
    make_ft_sensor(&fake.slaves[5]);

    args[0] = "device_probe";
    args[1] = "-o";
    args[2] = argv[1];
    args[3] = "-v";
    args[4] = argv[2];
    args[5] = "-t";
    args[6] = "1000";
    args[7] = NULL;
    CHECK_EQ(device_probe_main(7, args), 0);

    check_branch(argv[1], 0, "cia402");
    check_branch(argv[1], 1, "non_coe");
    check_branch(argv[1], 2, "infrastructure");
    check_branch(argv[1], 3, "mdp");
    check_branch(argv[1], 4, "cia402");
    check_branch(argv[1], 5, "other_profile");
    CHECK(contains(argv[1], 0, "\"od.identity_match\": true"));
    CHECK(!contains(argv[1], 0, "\"sii.pdo\""));
    CHECK(contains(argv[1], 0, "\"sii.coe_details\": {\"pdo_assign\": "
            "true, \"pdo_config\": true}"));
    CHECK(contains(argv[1], 1, "\"sii.coe_details\": {\"pdo_assign\": "
            "false, \"pdo_config\": false}"));
    CHECK(contains(argv[1], 3, "\"sii.coe_details\": null"));
    CHECK(contains(argv[1], 3, "\"od.rxpdo\": {\"basis\": \"assigned\", "
            "\"pdos\": []}"));
    CHECK(contains(argv[1], 3, "\"od.txpdo\""));
    CHECK(contains(argv[1], 3, "\"index\": \"0x6010\", \"subindex\": 1"));
    CHECK(contains(argv[1], 3, "\"sii.pdo\": {\"rxpdo\""));
    CHECK(contains(argv[1], 3, "\"name\": \"Channel 2\""));
    CHECK(contains(argv[1], 4, "\"od.profile_axis0\": {\"unavailable\": "
            "\"no_sdo_info\"}"));
    CHECK(contains(argv[1], 4, "\"od.rxpdo\""));
    CHECK(!contains(argv[1], 4, "\"od.txpdo\": {"));
    CHECK(contains(argv[1], 4, "\"data_type\": \"UNSIGNED16\", "
            "\"name\": \"Controlword\", \"source\": \"sii\""));
    CHECK(contains(argv[1], 4, "\"sii.pdo\": {\"txpdo\""));
    /* SM 0xFF PDOs are listed, marked "sm": null. */
    CHECK(contains(argv[1], 1, "\"index\": \"0x1a02\", \"name\": null, "
            "\"sm\": null"));
    CHECK(contains(argv[1], 5, "\"od.identity_match\": true"));
    CHECK(contains(argv[1], 5, "\"od.name_ver\": {\"device_name\": "
            "\"SimFT6\"}"));
    CHECK(contains(argv[1], 5, "\"name\": \"Force\", \"entry_name\": "
            "\"Force Y\""));
    CHECK(contains(argv[1], 5, "\"od.objects\": {\"objects\": ["));
    CHECK(contains(argv[1], 5, "\"total\": 4, \"truncated\": false"));
    CHECK(contains(argv[1], 5, "\"index\": \"0x1a01\", \"name\": "
            "\"Status\", \"sm\": null"));
    CHECK(!contains(argv[1], 5, "\"od.drive_modes\""));
    return TEST_RESULT();
}
