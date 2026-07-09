/*
 * Property-based tests for file_write() in
 * eval/extent/baseline/lowlevel_file.c
 *
 * Oracle: Algebraic - round-trip/invariant over the public page-table I/O
 * contract described in lowlevel_file.c's generated prompt: file_write should
 * split unaligned writes across pages, allocate/initialize pages as needed, and
 * file_read should observe exactly the written bytes while unrelated bytes are
 * preserved.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_WRITE_BYTES 2048U
#define MAX_TEST_PAGES 4U
#define MAX_TEST_PAGE 16U

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_WRITE_BYTES];
    unsigned char fill;
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
    wc->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        wc->page_off = 0U;
        break;
    case 1:
        wc->page_off = 1U;
        break;
    case 2:
        wc->page_off = PG_SIZE - 1U;
        break;
    case 3:
        wc->page_off = PG_SIZE - 16U;
        break;
    case 4:
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
        wc->data[i] = (unsigned char)theft_random_choice(t, 256U);
    }
    /* Keep the sparse-write round-trip oracle deterministic: len is always > 0. */
    wc->data[0] = (unsigned char)(1U + bounded_choice(t, 255U));
    wc->fill = (unsigned char)(1U + bounded_choice(t, 255U));

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
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, fill=%u}",
            wc->start_page, wc->page_count, wc->page_off, wc->len, wc->fill);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static struct indextb make_table(void)
{
    struct indextb tb;
    memset(&tb, 0, sizeof(tb));
    return tb;
}

static void destroy_table(struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
}

static int allocate_pages(struct indextb *tb, unsigned start_page,
                          unsigned page_count, unsigned char fill)
{
    for (unsigned i = 0; i < page_count; i++) {
        unsigned page = start_page + i;
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
        memset(tb->index[page], fill, PG_SIZE);
    }
    return 1;
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static unsigned case_extent_bytes(const struct write_case *wc)
{
    return wc->page_count * PG_SIZE;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    return (offset + len - 1U) / PG_SIZE;
}

static int read_back(struct indextb *tb, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(tb, offset, len, (char *)out);
    return 1;
}

static enum theft_trial_res prop_sparse_write_roundtrips(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(wc);

    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&tb, offset, wc->len, out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) {
        fprintf(stderr, "sparse write roundtrip mismatch: offset=%u len=%u first=%u got=%u\n",
                offset, wc->len, wc->data[0], out[0]);
    }

    free(out);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_allocated_write_preserves_unrelated_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(wc);
    unsigned extent_bytes = case_extent_bytes(wc);

    if (!allocate_pages(&tb, wc->start_page, wc->page_count, wc->fill)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&tb, wc->start_page * PG_SIZE, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned i = 0; i < wc->page_off; i++) {
        if (snapshot[i] != wc->fill) {
            fprintf(stderr, "prefix corrupted at %u got=%u expected=%u\n",
                    i, snapshot[i], wc->fill);
            ok = 0;
            break;
        }
    }
    if (ok && memcmp(snapshot + wc->page_off, wc->data, wc->len) != 0) {
        fprintf(stderr, "written span mismatch: offset=%u len=%u\n", offset, wc->len);
        ok = 0;
    }
    for (unsigned i = wc->page_off + wc->len; ok && i < extent_bytes; i++) {
        if (snapshot[i] != wc->fill) {
            fprintf(stderr, "suffix corrupted at %u got=%u expected=%u\n",
                    i, snapshot[i], wc->fill);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_data_allocates_zero_filled_range(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(wc);

    file_write(&tb, offset, wc->len, NULL);

    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, wc->len);
    for (unsigned page = start_page; page <= end_page; page++) {
        if (tb.index[page] == NULL) {
            fprintf(stderr, "NULL-data write failed to allocate page=%u offset=%u len=%u\n",
                    page, offset, wc->len);
            destroy_table(&tb);
            return THEFT_TRIAL_FAIL;
        }
    }

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&tb, offset, wc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "NULL-data write byte %u got=%u expected=0\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb = make_table();
    unsigned extent_bytes = case_extent_bytes(wc);
    unsigned offset = case_offset(wc);

    if (!allocate_pages(&tb, wc->start_page, wc->page_count, wc->fill)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *before = malloc(extent_bytes);
    unsigned char *after = malloc(extent_bytes);
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    read_back(&tb, wc->start_page * PG_SIZE, extent_bytes, before);
    file_write(&tb, offset, 0U, (const char *)wc->data);
    read_back(&tb, wc->start_page * PG_SIZE, extent_bytes, after);

    int ok = memcmp(before, after, extent_bytes) == 0;
    if (!ok) fprintf(stderr, "zero-length write changed data at offset=%u\n", offset);

    free(before);
    free(after);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &write_case_info },                         \
            .trials = trials_,                                         \
            .seed = theft_seed_of_time(),                              \
        };                                                             \
        enum theft_run_res res = theft_run(&cfg);                      \
        printf("  [%s] %s\n",                                        \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);     \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("baseline extent file_write property-based tests:\n");
    RUN_PROP("sparse_write_roundtrips", prop_sparse_write_roundtrips, 300);
    RUN_PROP("allocated_write_preserves_unrelated_bytes", prop_allocated_write_preserves_unrelated_bytes, 300);
    RUN_PROP("null_data_allocates_zero_filled_range", prop_null_data_allocates_zero_filled_range, 200);
    RUN_PROP("zero_length_write_is_noop", prop_zero_length_write_is_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
