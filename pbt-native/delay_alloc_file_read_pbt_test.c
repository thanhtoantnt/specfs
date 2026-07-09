/*
 * Property-based tests for file_read() in
 * eval/delay_alloc/optimization/lowlevel_file.c
 *
 * Oracle: Reference — bytewise projection over delayed-allocation pages
 * Stronger considered:
 *   - State Machine: rejected — file_read has no lifecycle or visible state transitions
 *   - Differential: rejected — no independent reference implementation of the same contract
 * Weaker available: Algebraic invariants, Crash-Only
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "delalloc.h"
#include "lowlevel_file.h"

#define MAX_CASE_PAGES 4U
#define MAX_TEST_PAGE 32U
#define MAX_READ_BYTES (MAX_CASE_PAGES * PAGE_SIZE)

extern struct alloc_buffer alloc_buffer;
extern int data_write_count;
extern int data_read_count;

struct read_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned present_mask;
    unsigned char page_data[MAX_CASE_PAGES * PAGE_SIZE];
    unsigned char data[MAX_READ_BYTES];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    rc->page_count = 1U + bounded_choice(t, MAX_CASE_PAGES);
    rc->start_page = bounded_choice(t, MAX_TEST_PAGE);

    switch (theft_random_choice(t, 6)) {
    case 0:
        rc->page_off = 0U;
        break;
    case 1:
        rc->page_off = 1U;
        break;
    case 2:
        rc->page_off = PAGE_SIZE - 1U;
        break;
    case 3:
        rc->page_off = PAGE_SIZE - 8U;
        break;
    case 4:
        rc->page_off = PAGE_SIZE / 2U;
        break;
    default:
        rc->page_off = bounded_choice(t, PAGE_SIZE);
        break;
    }

    unsigned span_bytes = rc->page_count * PAGE_SIZE - rc->page_off;
    unsigned max_len = span_bytes < MAX_READ_BYTES ? span_bytes : MAX_READ_BYTES;
    rc->len = 1U + bounded_choice(t, max_len);

    rc->present_mask = (unsigned)theft_random_choice(t, (uint64_t)(1U << rc->page_count));
    if (rc->present_mask == 0U) {
        rc->present_mask = 1U << bounded_choice(t, rc->page_count);
    }

    for (unsigned i = 0; i < sizeof(rc->page_data); i++) {
        rc->page_data[i] = (unsigned char)theft_random_choice(t, 256U);
    }
    for (unsigned i = 0; i < sizeof(rc->data); i++) {
        rc->data[i] = (unsigned char)theft_random_choice(t, 256U);
    }

    *instance = rc;
    return THEFT_ALLOC_OK;
}

static void read_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash read_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct read_case));
}

static void read_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct read_case *rc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, present_mask=0x%x}",
            rc->start_page, rc->page_count, rc->page_off, rc->len, rc->present_mask);
}

static struct theft_type_info read_case_info = {
    .alloc = read_case_alloc_cb,
    .free = read_case_free_cb,
    .hash = read_case_hash_cb,
    .print = read_case_print_cb,
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

static unsigned case_offset(const struct read_case *rc)
{
    return rc->start_page * PAGE_SIZE + rc->page_off;
}

static void read_region(struct indextb *tb, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(tb, offset, len, (char *)out);
}

static int populate_pages(struct indextb *tb, const struct read_case *rc)
{
    for (unsigned p = 0; p < rc->page_count; p++) {
        if (((rc->present_mask >> p) & 1U) == 0U) continue;

        unsigned page = rc->start_page + p;
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
        memcpy(tb->index[page], rc->page_data + (p * PAGE_SIZE), PAGE_SIZE);
    }
    return 1;
}

static unsigned char model_byte(const struct read_case *rc, unsigned absolute_offset)
{
    unsigned page = absolute_offset / PAGE_SIZE;
    if (page < rc->start_page || page >= rc->start_page + rc->page_count) return 0U;

    unsigned rel_page = page - rc->start_page;
    if (((rc->present_mask >> rel_page) & 1U) == 0U) return 0U;

    return rc->page_data[(rel_page * PAGE_SIZE) + (absolute_offset % PAGE_SIZE)];
}

/* Oracle: a read over sparse delayed-allocation storage returns the exact bytes
 * present in allocated pages and zero-fills holes. */
static enum theft_trial_res prop_sparse_layout_matches_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    init_table(&tb);

    if (!populate_pages(&tb, rc)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned offset = case_offset(rc);
    unsigned char *out = malloc(rc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, offset, rc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < rc->len; i++) {
        unsigned char expected = model_byte(rc, offset + i);
        if (out[i] != expected) {
            fprintf(stderr, "sparse read mismatch byte=%u got=%u expected=%u\n",
                    i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: reads observe buffered delayed-allocation writes exactly, including
 * writes that have not been flushed into tb->index yet. */
static enum theft_trial_res prop_buffered_write_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned offset = case_offset(rc);
    file_write(&tb, offset, rc->len, (const char *)rc->data);

    unsigned char *out = malloc(rc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, offset, rc->len, out);

    int ok = memcmp(out, rc->data, rc->len) == 0;
    if (!ok) {
        fprintf(stderr, "buffered roundtrip mismatch offset=%u len=%u\n", offset, rc->len);
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: a pristine table has no allocated pages, so every read returns zeroes. */
static enum theft_trial_res prop_unallocated_read_returns_zeroes(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned offset = case_offset(rc);
    unsigned char *out = malloc(rc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&tb, offset, rc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < rc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "unallocated read returned %u at byte=%u\n", out[i], i);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: zero-length reads are no-ops and do not touch the destination buffer. */
static enum theft_trial_res prop_zero_length_read_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    init_table(&tb);

    unsigned char out[16];
    unsigned char before[16];
    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(before));
    file_read(&tb, case_offset(rc), 0U, (char *)out);

    int ok = memcmp(out, before, sizeof(out)) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length read modified destination buffer\n");
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &read_case_info },
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

    printf("delay_alloc file_read property-based tests:\n");
    failures += run_prop("sparse_layout_matches_model", prop_sparse_layout_matches_model, 300);
    failures += run_prop("buffered_write_roundtrip_matches_input", prop_buffered_write_roundtrip_matches_input, 300);
    failures += run_prop("unallocated_read_returns_zeroes", prop_unallocated_read_returns_zeroes, 200);
    failures += run_prop("zero_length_read_is_noop", prop_zero_length_read_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
