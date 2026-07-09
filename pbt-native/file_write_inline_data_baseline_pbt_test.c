/*
 * Property-based tests for file_write() in
 * eval/inline_data/baseline/lowlevel_file.c
 *
 * Oracle: algebraic round-trip/invariant over the page-table I/O contract in
 * lowlevel_file.c's generated prompt. file_write should split unaligned writes
 * across pages, allocate initialized pages as needed, make file_read observe
 * exactly the written bytes, and preserve unrelated bytes.
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

struct counter cnt = { .size = 0 };

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *write_case = malloc(sizeof(*write_case));
    if (write_case == NULL) return THEFT_ALLOC_ERROR;

    write_case->start_page = bounded_choice(t, MAX_TEST_PAGE);
    write_case->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        write_case->page_off = 0U;
        break;
    case 1:
        write_case->page_off = 1U;
        break;
    case 2:
        write_case->page_off = PG_SIZE - 1U;
        break;
    case 3:
        write_case->page_off = PG_SIZE - 16U;
        break;
    case 4:
        write_case->page_off = PG_SIZE / 2U;
        break;
    default:
        write_case->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = write_case->page_count * PG_SIZE - write_case->page_off;
    unsigned max_len = span_bytes < MAX_WRITE_BYTES ? span_bytes : MAX_WRITE_BYTES;
    write_case->len = 1U + bounded_choice(t, max_len);

    for (unsigned byte_idx = 0; byte_idx < MAX_WRITE_BYTES; byte_idx++) {
        write_case->data[byte_idx] = (unsigned char)theft_random_choice(t, 256U);
    }
    write_case->data[0] = (unsigned char)(1U + bounded_choice(t, 255U));
    write_case->fill = (unsigned char)(1U + bounded_choice(t, 255U));

    *instance = write_case;
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

static void write_case_print_cb(FILE *file, const void *instance, void *env)
{
    (void)env;
    const struct write_case *write_case = instance;
    fprintf(file, "{start_page=%u, page_count=%u, page_off=%u, len=%u, fill=%u}",
            write_case->start_page, write_case->page_count, write_case->page_off,
            write_case->len, write_case->fill);
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
    for (unsigned page_idx = 0; page_idx < INDEXTB_NUM; page_idx++) {
        free(tb->index[page_idx]);
        tb->index[page_idx] = NULL;
    }
}

static int allocate_pages(struct indextb *tb, unsigned start_page,
                          unsigned page_count, unsigned char fill)
{
    for (unsigned page_idx = 0; page_idx < page_count; page_idx++) {
        unsigned page = start_page + page_idx;
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
        memset(tb->index[page], fill, PG_SIZE);
    }
    return 1;
}

static unsigned case_offset(const struct write_case *write_case)
{
    return write_case->start_page * PG_SIZE + write_case->page_off;
}

static unsigned case_extent_bytes(const struct write_case *write_case)
{
    return write_case->page_count * PG_SIZE;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    return (offset + len - 1U) / PG_SIZE;
}

static void read_back(struct indextb *tb, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(tb, offset, len, (char *)out);
}

static enum theft_trial_res prop_sparse_write_roundtrips(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *write_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(write_case);

    file_write(&tb, offset, write_case->len, (const char *)write_case->data);

    unsigned char *out = malloc(write_case->len);
    if (out == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&tb, offset, write_case->len, out);

    int ok = memcmp(out, write_case->data, write_case->len) == 0;
    if (!ok) {
        fprintf(stderr, "sparse write roundtrip mismatch: offset=%u len=%u first=%u got=%u\n",
                offset, write_case->len, write_case->data[0], out[0]);
    }

    free(out);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_write_allocates_touched_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *write_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(write_case);
    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, write_case->len);

    file_write(&tb, offset, write_case->len, (const char *)write_case->data);

    int ok = 1;
    for (unsigned page = start_page; page <= end_page; page++) {
        if (tb.index[page] == NULL) {
            fprintf(stderr, "sparse write did not allocate page=%u offset=%u len=%u\n",
                    page, offset, write_case->len);
            ok = 0;
            break;
        }
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_allocated_write_preserves_unrelated_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *write_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(write_case);
    unsigned extent_bytes = case_extent_bytes(write_case);

    if (!allocate_pages(&tb, write_case->start_page, write_case->page_count, write_case->fill)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    file_write(&tb, offset, write_case->len, (const char *)write_case->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&tb, write_case->start_page * PG_SIZE, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned byte_idx = 0; byte_idx < write_case->page_off; byte_idx++) {
        if (snapshot[byte_idx] != write_case->fill) {
            fprintf(stderr, "prefix corrupted at %u got=%u expected=%u\n",
                    byte_idx, snapshot[byte_idx], write_case->fill);
            ok = 0;
            break;
        }
    }
    if (ok && memcmp(snapshot + write_case->page_off, write_case->data, write_case->len) != 0) {
        fprintf(stderr, "written span mismatch: offset=%u len=%u\n", offset, write_case->len);
        ok = 0;
    }
    for (unsigned byte_idx = write_case->page_off + write_case->len;
         ok && byte_idx < extent_bytes;
         byte_idx++) {
        if (snapshot[byte_idx] != write_case->fill) {
            fprintf(stderr, "suffix corrupted at %u got=%u expected=%u\n",
                    byte_idx, snapshot[byte_idx], write_case->fill);
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
    const struct write_case *write_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(write_case);

    file_write(&tb, offset, write_case->len, NULL);

    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, write_case->len);
    for (unsigned page = start_page; page <= end_page; page++) {
        if (tb.index[page] == NULL) {
            fprintf(stderr, "NULL-data write failed to allocate page=%u offset=%u len=%u\n",
                    page, offset, write_case->len);
            destroy_table(&tb);
            return THEFT_TRIAL_FAIL;
        }
    }

    unsigned char *out = malloc(write_case->len);
    if (out == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&tb, offset, write_case->len, out);

    int ok = 1;
    for (unsigned byte_idx = 0; byte_idx < write_case->len; byte_idx++) {
        if (out[byte_idx] != 0U) {
            fprintf(stderr, "NULL-data write byte %u got=%u expected=0\n",
                    byte_idx, out[byte_idx]);
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
    const struct write_case *write_case = arg1;
    struct indextb tb = make_table();
    unsigned extent_bytes = case_extent_bytes(write_case);
    unsigned offset = case_offset(write_case);

    if (!allocate_pages(&tb, write_case->start_page, write_case->page_count, write_case->fill)) {
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

    read_back(&tb, write_case->start_page * PG_SIZE, extent_bytes, before);
    file_write(&tb, offset, 0U, (const char *)write_case->data);
    read_back(&tb, write_case->start_page * PG_SIZE, extent_bytes, after);

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

    printf("inline_data baseline file_write property-based tests:\n");
    RUN_PROP("sparse_write_roundtrips", prop_sparse_write_roundtrips, 300);
    RUN_PROP("sparse_write_allocates_touched_pages", prop_sparse_write_allocates_touched_pages, 300);
    RUN_PROP("allocated_write_preserves_unrelated_bytes", prop_allocated_write_preserves_unrelated_bytes, 300);
    RUN_PROP("null_data_allocates_zero_filled_range", prop_null_data_allocates_zero_filled_range, 200);
    RUN_PROP("zero_length_write_is_noop", prop_zero_length_write_is_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
