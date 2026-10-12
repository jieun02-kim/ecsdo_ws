/*
 * SII read (core, fake ioctl) and SII category parser tests.
 */

#include <errno.h>
#include <stdlib.h>

#include "ecsdo/ecsdo.h"
#include "ecsdo/ecsdo_sii.h"
#include "ecsdo_ioctl.h"
#include "fake_ioctl.h"
#include "sii_img.h"
#include "test_util.h"

/** Digital input terminal: no mailbox, SM0 = process data in, two TxPDOs
 *  "Channel 1/2" with one "Input" BOOLEAN each, and an RxPDO not assigned
 *  by default (SM 0xFF). */
static void make_terminal(sii_img_t *s)
{
    static const char *const str[] = {
        "EL1002", "DigIn", "Channel 1", "Channel 2", "Input",
    };

    img_start(s);
    strings(s, str, 5);
    cat_begin(s, 0x0029);
    sm(s, 0x1000, 1, ECSDO_SII_SM_PD_IN);
    cat_end(s);
    cat_begin(s, 0x0032);
    pdo(s, 0x1A00, 1, 0, 3);
    entry(s, 0x6000, 1, 5, 0x0001, 1);
    pdo(s, 0x1A01, 1, 0, 4);
    entry(s, 0x6010, 1, 5, 0x0001, 1);
    cat_end(s);
    cat_begin(s, 0x0033);
    pdo(s, 0x1600, 1, 0xFF, 0);
    entry(s, 0x7000, 1, 0, 0x0001, 1);
    cat_end(s);
    img_end(s);
}

/* ------------------------------------------------------------------------
 * Parser
 * ---------------------------------------------------------------------- */

static void test_parse_terminal(void)
{
    sii_img_t img;
    ecsdo_sii_t *sii;

    make_terminal(&img);
    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, &sii), ECSDO_OK);
    CHECK(sii->has_categories);
    CHECK(!sii->incomplete);
    CHECK_EQ(sii->sync_count, 1);
    CHECK_EQ(sii->syncs[0].start_address, 0x1000);
    CHECK_EQ(sii->syncs[0].type, ECSDO_SII_SM_PD_IN);

    CHECK_EQ(sii->pdo_count, 3);
    CHECK_EQ(sii->pdos[0].index, 0x1A00);
    CHECK_EQ(sii->pdos[0].dir, ECSDO_SII_TXPDO);
    CHECK_EQ(sii->pdos[0].sync_index, 0);
    CHECK_STR(sii->pdos[0].name, "Channel 1");
    CHECK_EQ(sii->pdos[0].entry_count, 1);
    CHECK_EQ(sii->pdos[0].entries[0].index, 0x6000);
    CHECK_EQ(sii->pdos[0].entries[0].subindex, 1);
    CHECK_EQ(sii->pdos[0].entries[0].data_type, 0x0001);
    CHECK_EQ(sii->pdos[0].entries[0].bit_length, 1);
    CHECK_STR(sii->pdos[0].entries[0].name, "Input");
    CHECK_STR(sii->pdos[1].name, "Channel 2");

    CHECK_EQ(sii->pdos[2].dir, ECSDO_SII_RXPDO);
    CHECK_EQ(sii->pdos[2].sync_index, -1);
    CHECK_STR(sii->pdos[2].name, "");
    CHECK_STR(sii->pdos[2].entries[0].name, "");

    CHECK(ecsdo_sii_has_process_data(sii));
    ecsdo_sii_free(sii);
}

/** Strings after the categories that refer to them still resolve. */
static void test_parse_strings_last(void)
{
    static const char *const str[] = {"Statusword"};
    sii_img_t img;
    ecsdo_sii_t *sii;

    img_start(&img);
    cat_begin(&img, 0x0032);
    pdo(&img, 0x1A00, 1, 3, 0);
    entry(&img, 0x6041, 0, 1, 0x0006, 16);
    cat_end(&img);
    strings(&img, str, 1);
    img_end(&img);

    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, &sii), ECSDO_OK);
    CHECK_EQ(sii->pdo_count, 1);
    CHECK_STR(sii->pdos[0].entries[0].name, "Statusword");
    CHECK_EQ(sii->pdos[0].entries[0].data_type, 0x0006);
    ecsdo_sii_free(sii);
}

/** Coupler: strings and general only -> no process data. Vendor-specific
 *  category bit (0x8000) is masked like the master does. */
static void test_parse_no_process_data(void)
{
    static const char *const str[] = {"EK1100"};
    sii_img_t img;
    ecsdo_sii_t *sii;

    img_start(&img);
    strings(&img, str, 1);
    cat_begin(&img, 0x001E);
    put16(&img, 0);
    cat_end(&img);
    cat_begin(&img, 0x8029);         /* vendor-specific SyncM */
    sm(&img, 0x1000, 0, ECSDO_SII_SM_MBX_OUT);
    cat_end(&img);
    img_end(&img);

    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, &sii), ECSDO_OK);
    CHECK_EQ(sii->pdo_count, 0);
    CHECK_EQ(sii->sync_count, 1);
    CHECK(!ecsdo_sii_has_process_data(sii));
    ecsdo_sii_free(sii);

    /* Fixed area only. */
    CHECK_EQ(ecsdo_sii_parse(img.b, 0x40 * 2, &sii), ECSDO_OK);
    CHECK(!sii->has_categories);
    CHECK(!ecsdo_sii_has_process_data(sii));
    ecsdo_sii_free(sii);

    /* SM type alone counts as process data. */
    img_start(&img);
    cat_begin(&img, 0x0029);
    sm(&img, 0x1100, 0, ECSDO_SII_SM_PD_OUT);
    cat_end(&img);
    img_end(&img);
    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, &sii), ECSDO_OK);
    CHECK(ecsdo_sii_has_process_data(sii));
    ecsdo_sii_free(sii);
}

static void test_parse_malformed(void)
{
    sii_img_t img;
    ecsdo_sii_t *sii;

    /* Category size beyond the end: stop, keep nothing from it. */
    make_terminal(&img);
    CHECK_EQ(ecsdo_sii_parse(img.b, img.n - 12, &sii), ECSDO_OK);
    CHECK(sii->incomplete);
    CHECK_EQ(sii->pdo_count, 2);     /* TxPDOs before the cut */
    ecsdo_sii_free(sii);

    /* PDO claims more entries than the category holds. */
    img_start(&img);
    cat_begin(&img, 0x0032);
    pdo(&img, 0x1A00, 3, 3, 0);
    entry(&img, 0x6041, 0, 0, 0x0006, 16);
    cat_end(&img);
    img_end(&img);
    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, &sii), ECSDO_OK);
    CHECK(sii->incomplete);
    CHECK_EQ(sii->pdo_count, 1);
    CHECK_EQ(sii->pdos[0].entry_count, 0);
    ecsdo_sii_free(sii);

    /* No end marker and nothing more. */
    img_start(&img);
    put8(&img, 0x0A);
    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, &sii), ECSDO_OK);
    CHECK(sii->incomplete);
    ecsdo_sii_free(sii);

    CHECK_EQ(ecsdo_sii_parse(NULL, 4, &sii), ECSDO_ERR_INVALID_ARG);
    CHECK_EQ(ecsdo_sii_parse(img.b, img.n, NULL), ECSDO_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------------
 * Core: ecsdo_read_sii_info / ecsdo_read_sii
 * ---------------------------------------------------------------------- */

static void test_read_sii(void)
{
    static sii_img_t img;
    ecsdo_sii_info_t info;
    ecsdo_ctx_t *ctx;
    uint8_t *data;
    size_t size;

    make_terminal(&img);
    fake_reset();
    fake.slave_count = 2;
    fake.slaves[0].mailbox_protocols = ECSDO_MBOX_COE | ECSDO_MBOX_FOE;
    fake.slaves[0].has_general_category = 1;
    fake.slaves[0].enable_sdo_info = 1;
    fake.slaves[0].order = "L7NHA004U";
    fake.slaves[0].group = "Servo Drive";
    fake.slaves[0].sii = img.b;
    fake.slaves[0].sii_size = img.n;
    fake.rw_open_errno = EACCES;     /* SII read works read-only */

    CHECK_EQ(ecsdo_open(&ctx, 0), ECSDO_OK);
    CHECK_EQ(ecsdo_read_sii_info(ctx, 0, &info), ECSDO_OK);
    CHECK_EQ(info.mailbox_protocols, ECSDO_MBOX_COE | ECSDO_MBOX_FOE);
    CHECK_EQ(info.has_general_category, 1);
    CHECK_EQ(info.enable_sdo_info, 1);
    CHECK_STR(info.order, "L7NHA004U");
    CHECK_STR(info.group, "Servo Drive");
    CHECK_EQ(info.sii_nwords, img.n / 2);

    CHECK_EQ(ecsdo_read_sii(ctx, 0, &data, &size), ECSDO_OK);
    CHECK_EQ(size, img.n);
    CHECK(data && !memcmp(data, img.b, img.n));
    free(data);

    /* No SII held by the master. */
    CHECK_EQ(ecsdo_read_sii(ctx, 1, &data, &size), ECSDO_OK);
    CHECK(!data);
    CHECK_EQ(size, 0);

    CHECK_EQ(ecsdo_read_sii(ctx, 5, &data, &size), ECSDO_ERR_NO_SLAVE);
    CHECK_EQ(ecsdo_read_sii_info(ctx, 5, &info), ECSDO_ERR_NO_SLAVE);
    CHECK_EQ(ecsdo_read_sii(ctx, 0, NULL, &size), ECSDO_ERR_INVALID_ARG);
    ecsdo_close(ctx);
}

int main(void)
{
    RUN(test_parse_terminal);
    RUN(test_parse_strings_last);
    RUN(test_parse_no_process_data);
    RUN(test_parse_malformed);
    RUN(test_read_sii);
    return TEST_RESULT();
}
