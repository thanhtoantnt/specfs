/*
 * Property-based tests for file_write() in
 * eval/delay_alloc/optimization/lowlevel_file.c
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "delalloc.h"
#include "lowlevel_file.h"

#define MAX_WRITE_BYTES 8192U
#define MAX_CASE_PAGES 4U
#define MAX_TEST_PAGE 32U

extern struct alloc_buffer alloc_buffer;
extern int data_write_count;
extern int data_read_count;

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_WRITE_BYTES];
    unsigned char data2[MAX_WRITE_BYTES];
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

    wc->page_count = 1U + bounded_choice(t, MAX_CASE_PAGES);
    wc->start_page = bounded_choice(t, MAX_TEST_PAGE);

    switch (theft_random_choice(t, 6)) {
    case 0:
        wc->page_off = 0U;
        break;
    case 1:
        wc->page_off = 1U;
        break;
    case 2:
        wc->page_off = PAGE_SIZE - 1U;
        break;
    case 3:
        wc->page_off = PAGE_SIZE - 8U;
        break;
    case 4:
        wc->page_off = PAGE_SIZE / 2U;
        break;
    default:
        wc->page_off = bounded_choice(t, PAGE_SIZE);
        break;
    }

    unsigned span_bytes = wc->page_count * PAGE_SIZE - wc->page_off;
    unsigned max_len = span_bytes < MAX_WRITE_BYTES ? span_bytes : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256);
        wc->data2[i] = (unsigned char)theft_random_choice(t, 256);
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

void buffer_lock(struct alloc_buffer *buf)
{
    (void)buf;
}

void buffer_unlock(struct alloc_buffer *buf)
{
    (void)buf;
}

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PAGE_SIZE);
    if (page != NULL) memset(page, 0, PAGE_SIZE);
    return page;
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PAGE_SIZE + wc->page_off;
}

static unsigned case_region_bytes(const struct write_case *wc)
{
    return wc->page_count * PAGE_SIZE;
}

static void reset_delalloc_state(void)
{
    memset(&alloc_buffer, 0, sizeof(alloc_buffer));
    data_write_count = 0;
    data_read_count = 0;
}

static void init_table(struct indextb *tb)
{
    reset_delalloc_state();
    memset(tb, 0, sizeof(*tb));
}

static void cleanup_table(struct indextb *tb)
{
    brels(tb);
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
    reset_delalloc_state();
}

static int prefill_pages(struct indextb *tb, unsigned start_page, unsigned page_count, unsigned char fill)
{
    for (unsigned p = 0; p < page_count; p++) {
        unsigned page = start_page + p;
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
        memset(tb->index[page], fill, PAGE_SIZE);
    }
    return 1;
}

static int read_region(struct indextb *tb, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(tb, offset, len, (char *)out);
    return 1;
}

/* Oracle: after a valid-range file_write(tb, offset, len, data), file_read of the
 * same byte range returns exactly data. */
static enum theft_trial_res prop_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, offset, wc->len, out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) fprintf(stderr, "roundtrip mismatch offset=%u len=%u\n", offset, wc->len);

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: writing into sparse/unallocated delayed-allocation storage creates a
 * logical region where unwritten bytes read as zero and written bytes match data. */
static enum theft_trial_res prop_sparse_write_zero_fills_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned offset = case_offset(wc);
    unsigned region = case_region_bytes(wc);
    unsigned region_start = wc->start_page * PAGE_SIZE;
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, region_start, region, out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned abs_off = region_start + i;
        unsigned char expected = 0U;
        if (abs_off >= offset && abs_off < offset + wc->len) {
            expected = wc->data[abs_off - offset];
        }
        if (out[i] != expected) {
            fprintf(stderr, "sparse mismatch byte=%u got=%u expected=%u\n", i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: on pre-existing pages, file_write mutates only the requested byte
 * range and preserves all bytes before and after the write span. */
static enum theft_trial_res prop_existing_pages_preserve_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned char fill = 0xA5;
    if (!prefill_pages(&tb, wc->start_page, wc->page_count, fill)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned offset = case_offset(wc);
    unsigned region = case_region_bytes(wc);
    unsigned region_start = wc->start_page * PAGE_SIZE;
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, region_start, region, out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned abs_off = region_start + i;
        unsigned char expected = fill;
        if (abs_off >= offset && abs_off < offset + wc->len) {
            expected = wc->data[abs_off - offset];
        }
        if (out[i] != expected) {
            fprintf(stderr, "preservation mismatch byte=%u got=%u expected=%u\n", i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: the clear_file contract is implemented by file_write(..., NULL), so
 * NULL data zero-fills the requested range instead of crashing or no-oping. */
static enum theft_trial_res prop_null_data_zero_fills(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb;
    init_table(&tb);

    if (!prefill_pages(&tb, wc->start_page, wc->page_count, 0x7BU)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, NULL);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, offset, wc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "zero-fill mismatch byte=%u got=%u\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: a zero-length write is a no-op: it does not enqueue delayed writes
 * and does not change existing page contents. */
static enum theft_trial_res prop_zero_length_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned region = case_region_bytes(wc);
    unsigned region_start = wc->start_page * PAGE_SIZE;
    unsigned char fill = 0x5CU;
    if (!prefill_pages(&tb, wc->start_page, wc->page_count, fill)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    file_write(&tb, case_offset(wc), 0U, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, region_start, region, out);

    int ok = alloc_buffer.size == 0U;
    for (unsigned i = 0; ok && i < region; i++) {
        if (out[i] != fill) {
            fprintf(stderr, "zero-length changed byte=%u got=%u\n", i, out[i]);
            ok = 0;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: sequential overlapping writes obey last-writer-wins semantics across
 * delayed allocation buffering and page boundaries. */
static enum theft_trial_res prop_overlapping_writes_are_last_writer_wins(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned offset = case_offset(wc);
    unsigned delta = wc->len / 2U;
    unsigned second_len = wc->len - delta;
    file_write(&tb, offset, wc->len, (const char *)wc->data);
    file_write(&tb, offset + delta, second_len, (const char *)wc->data2);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, offset, wc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        unsigned char expected = i < delta ? wc->data[i] : wc->data2[i - delta];
        if (out[i] != expected) {
            fprintf(stderr, "last-writer mismatch byte=%u got=%u expected=%u\n", i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &write_case_info },
        .trials = trials,
        .seed = theft_seed_of_time(),
    };
    enum theft_run_res res = theft_run(&cfg);
    printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    printf("delay_alloc lowlevel file_write property-based tests:\n");
    failures += run_prop("roundtrip_matches_input", prop_roundtrip_matches_input, 300);
    failures += run_prop("sparse_write_zero_fills_unwritten_bytes", prop_sparse_write_zero_fills_unwritten_bytes, 300);
    failures += run_prop("existing_pages_preserve_unwritten_bytes", prop_existing_pages_preserve_unwritten_bytes, 300);
    failures += run_prop("null_data_zero_fills", prop_null_data_zero_fills, 200);
    failures += run_prop("zero_length_write_is_noop", prop_zero_length_write_is_noop, 100);
    failures += run_prop("overlapping_writes_are_last_writer_wins", prop_overlapping_writes_are_last_writer_wins, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
