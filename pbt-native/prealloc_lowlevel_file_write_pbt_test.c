/*
 * Property-based tests for file_write() in
 * eval/pre_alloc/optimization/lowlevel_file.c
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_WRITE_BYTES 2048U
#define MAX_PAGES 4U
#define MAX_TEST_PAGE 16U

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_WRITE_BYTES];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    wc->start_page = bounded_choice(t, MAX_TEST_PAGE);
    wc->page_count = 1U + bounded_choice(t, MAX_PAGES);

    switch (theft_random_choice(t, 5)) {
    case 0:
        wc->page_off = 0U;
        break;
    case 1:
        wc->page_off = PG_SIZE - 1U;
        break;
    case 2:
        wc->page_off = PG_SIZE - 8U;
        break;
    case 3:
        wc->page_off = PG_SIZE / 2U;
        break;
    default:
        wc->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = wc->page_count * PG_SIZE - wc->page_off;
    unsigned max_len = span_bytes < MAX_WRITE_BYTES ? span_bytes : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256);
    }

    *instance = wc;
    return THEFT_ALLOC_OK;
}

static void write_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash write_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct write_case));
}

static void write_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct write_case *wc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u}",
            wc->start_page, wc->page_count, wc->page_off, wc->len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static unsigned case_extent_bytes(const struct write_case *wc)
{
    return wc->page_count * PG_SIZE;
}

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
}

unsigned char *malloc_contigous_pages(unsigned num)
{
    if (num == 0U) return NULL;
    unsigned char *data = malloc(num * PG_SIZE);
    if (data != NULL) memset(data, 0, num * PG_SIZE);
    return data;
}

static struct Extent *make_extent(unsigned start_page, unsigned page_count, unsigned char fill)
{
    struct Extent *ext = malloc(sizeof(*ext));
    if (ext == NULL) return NULL;

    ext->start_page = start_page;
    ext->length = page_count;
    ext->data = malloc(page_count * PG_SIZE);
    if (ext->data == NULL) {
        free(ext);
        return NULL;
    }
    memset(ext->data, fill, page_count * PG_SIZE);
    ext->next = NULL;
    return ext;
}

static void free_extents(struct inode *node)
{
    struct Extent *cur = node->extents;
    while (cur != NULL) {
        struct Extent *next = cur->next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    node->extents = NULL;
}

static int read_back(struct inode *node, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(node, offset, len, (char *)out);
    return 1;
}

static enum theft_trial_res prop_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);

    file_write(&node, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, offset, wc->len, out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) fprintf(stderr, "roundtrip mismatch offset=%u len=%u\n", offset, wc->len);

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_preserves_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned extent_bytes = case_extent_bytes(wc);
    unsigned offset = case_offset(wc);
    unsigned char fill = 0xA5;

    node.extents = make_extent(wc->start_page, wc->page_count, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, offset, wc->len, (const char *)wc->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, wc->start_page * PG_SIZE, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned i = 0; i < wc->page_off; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "prefix corrupted at %u\n", i);
            ok = 0;
            break;
        }
    }
    if (ok && memcmp(snapshot + wc->page_off, wc->data, wc->len) != 0) {
        fprintf(stderr, "write span mismatch offset=%u len=%u\n", offset, wc->len);
        ok = 0;
    }
    for (unsigned i = wc->page_off + wc->len; ok && i < extent_bytes; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "suffix corrupted at %u\n", i);
            ok = 0;
        }
    }

    free(snapshot);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_data_zero_fills(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);

    node.extents = make_extent(wc->start_page, wc->page_count, 0x7B);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, offset, wc->len, NULL);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, offset, wc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "zero-fill mismatch at %u got=%u\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned extent_bytes = case_extent_bytes(wc);
    unsigned offset = case_offset(wc);
    unsigned char fill = 0x5C;

    node.extents = make_extent(wc->start_page, wc->page_count, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, offset, 0, (const char *)wc->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, wc->start_page * PG_SIZE, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned i = 0; i < extent_bytes; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "zero-length write changed byte %u\n", i);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_out_of_range_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned char fill = 0x33;
    unsigned extent_bytes = PG_SIZE;

    node.extents = make_extent(0, 1, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, MAX_FILE_SIZE + wc->page_off, wc->len, (const char *)wc->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, 0, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned i = 0; i < extent_bytes; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "out-of-range write changed byte %u\n", i);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &write_case_info },
        .trials = 200,
    };
    enum theft_run_res res = theft_run(&cfg);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

static enum theft_trial_res prop_partial_out_of_range_is_noop(struct theft *t, void *arg1);

int main(void)
{
    int failures = 0;
    failures += run_prop("file_write roundtrip matches input", prop_roundtrip_matches_input);
    failures += run_prop("file_write preserves unwritten bytes", prop_preserves_unwritten_bytes);
    failures += run_prop("file_write NULL data zero-fills", prop_null_data_zero_fills);
    failures += run_prop("file_write zero length is noop", prop_zero_length_noop);
    failures += run_prop("file_write out-of-range is noop", prop_out_of_range_write_is_noop);
    failures += run_prop("partial out-of-range is noop", prop_partial_out_of_range_is_noop);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * BugDocs: partial out-of-range write — offset < MAX_FILE_SIZE but
 * offset + len > MAX_FILE_SIZE. file_allocate rejects, but the copy
 * loop still writes to pages in the in-range prefix before hitting
 * page >= INDEXTB_NUM.
 */
static enum theft_trial_res prop_partial_out_of_range_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();

    /* Allocate the last page so it exists for writes to hit */
    unsigned last_page = INDEXTB_NUM - 1;
    unsigned char fill = 0xAB;
    node.extents = make_extent(last_page, 1, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    /* Snapshot before */
    unsigned char before[PG_SIZE];
    read_back(&node, last_page * PG_SIZE, PG_SIZE, before);

    /* Write that starts in-range but extends past MAX_FILE_SIZE */
    unsigned offset = (last_page * PG_SIZE) + (wc->page_off % PG_SIZE);
    unsigned len = PG_SIZE * 2;  /* extends past MAX_FILE_SIZE */
    file_write(&node, offset, len, (const char *)wc->data);

    /* Snapshot after — should be unchanged (write was out of range) */
    unsigned char after[PG_SIZE];
    read_back(&node, last_page * PG_SIZE, PG_SIZE, after);

    int ok = (memcmp(before, after, PG_SIZE) == 0);
    if (!ok) {
        fprintf(stderr, "partial_out_of_range FAIL: last page mutated by partial write\n");
    }

    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}
