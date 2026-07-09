/*
 * Property-based tests for clear_file() in
 * eval/delay_alloc/optimization/lowlevel_file.c
 *
 * Oracle: Algebraic — Invariant (4d)
 * Stronger considered:
 *   - State Machine (3): rejected — clear_file is a single-call mutation over inode data.
 *   - Differential (7): rejected — no independent delayed-allocation implementation is present.
 *   - Round-trip (4a): rejected — clearing is intentionally lossy.
 * Weaker available: Negative/Error Contract (4e), Crash-Only (6)
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "delalloc.h"
#include "lowlevel_file.h"

#define MAX_CLEAR_BYTES 8192U
#define MAX_CASE_PAGES 4U
#define MAX_TEST_PAGE 32U

extern struct alloc_buffer alloc_buffer;
extern int data_write_count;
extern int data_read_count;

struct clear_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char fill;
    unsigned char data[MAX_CLEAR_BYTES];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res clear_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct clear_case *cc = malloc(sizeof(*cc));
    if (cc == NULL) return THEFT_ALLOC_ERROR;

    cc->page_count = 1U + bounded_choice(t, MAX_CASE_PAGES);
    cc->start_page = bounded_choice(t, MAX_TEST_PAGE);

    switch (theft_random_choice(t, 6)) {
    case 0:
        cc->page_off = 0U;
        break;
    case 1:
        cc->page_off = 1U;
        break;
    case 2:
        cc->page_off = PAGE_SIZE - 1U;
        break;
    case 3:
        cc->page_off = PAGE_SIZE - 8U;
        break;
    case 4:
        cc->page_off = PAGE_SIZE / 2U;
        break;
    default:
        cc->page_off = bounded_choice(t, PAGE_SIZE);
        break;
    }

    unsigned span_bytes = cc->page_count * PAGE_SIZE - cc->page_off;
    unsigned max_len = span_bytes < MAX_CLEAR_BYTES ? span_bytes : MAX_CLEAR_BYTES;
    cc->len = 1U + bounded_choice(t, max_len);
    cc->fill = (unsigned char)theft_random_choice(t, 256U);

    for (unsigned i = 0; i < MAX_CLEAR_BYTES; i++) {
        cc->data[i] = (unsigned char)theft_random_choice(t, 256U);
    }

    *instance = cc;
    return THEFT_ALLOC_OK;
}

static void clear_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash clear_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct clear_case));
}

static void clear_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct clear_case *cc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, fill=%u}",
            cc->start_page, cc->page_count, cc->page_off, cc->len, cc->fill);
}

static struct theft_type_info clear_case_info = {
    .alloc = clear_case_alloc_cb,
    .free = clear_case_free_cb,
    .hash = clear_case_hash_cb,
    .print = clear_case_print_cb,
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

static void init_inode(struct inode *node, struct indextb *tb)
{
    reset_delalloc_state();
    memset(tb, 0, sizeof(*tb));
    memset(node, 0, sizeof(*node));
    node->file = tb;
}

static void cleanup_inode(struct inode *node)
{
    struct indextb *tb = node->file;
    if (tb != NULL) {
        brels(tb);
        for (unsigned i = 0; i < INDEXTB_NUM; i++) {
            free(tb->index[i]);
            tb->index[i] = NULL;
        }
    }
    reset_delalloc_state();
}

static unsigned case_offset(const struct clear_case *cc)
{
    return cc->start_page * PAGE_SIZE + cc->page_off;
}

static unsigned case_region_bytes(const struct clear_case *cc)
{
    return cc->page_count * PAGE_SIZE;
}

static unsigned touched_page_count(unsigned offset, unsigned len)
{
    if (len == 0U) return 0U;
    unsigned first = offset / PAGE_SIZE;
    unsigned last = (offset + len - 1U) / PAGE_SIZE;
    return last - first + 1U;
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

static void read_region(struct inode *node, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(node->file, offset, len, (char *)out);
}

/* Oracle: clear_file(node, start, len) zeroes exactly the requested byte span
 * across allocated pages and preserves bytes outside the span. */
static enum theft_trial_res prop_clear_zeros_target_span(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    if (!prefill_pages(&tb, cc->start_page, cc->page_count, cc->fill)) {
        cleanup_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    unsigned offset = case_offset(cc);
    unsigned region = case_region_bytes(cc);
    unsigned region_start = cc->start_page * PAGE_SIZE;
    clear_file(&node, offset, cc->len);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&node, region_start, region, out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned abs_off = region_start + i;
        unsigned char expected = cc->fill;
        if (abs_off >= offset && abs_off < offset + cc->len) {
            expected = 0U;
        }
        if (out[i] != expected) {
            fprintf(stderr, "clear mismatch byte=%u got=%u expected=%u offset=%u len=%u\n",
                    i, out[i], expected, offset, cc->len);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: clearing sparse delayed-allocation storage creates pending zeroed
 * records for every page in the cleared range. */
static enum theft_trial_res prop_sparse_clear_enqueues_touched_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    unsigned offset = case_offset(cc);
    clear_file(&node, offset, cc->len);

    unsigned expected_pages = touched_page_count(offset, cc->len);
    int ok = alloc_buffer.size == expected_pages;
    if (!ok) {
        fprintf(stderr, "expected %u delayed records, got %u (offset=%u len=%u)\n",
                expected_pages, alloc_buffer.size, offset, cc->len);
    }

    unsigned first_page = offset / PAGE_SIZE;
    for (unsigned i = 0; ok && i < expected_pages; i++) {
        unsigned expected_page = first_page + i;
        if (alloc_buffer.rec[i].tb != &tb || alloc_buffer.rec[i].page != expected_page) {
            fprintf(stderr, "record %u points at tb=%p page=%u, expected tb=%p page=%u\n",
                    i, (void *)alloc_buffer.rec[i].tb, alloc_buffer.rec[i].page,
                    (void *)&tb, expected_page);
            ok = 0;
        }
    }

    cleanup_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: clearing sparse storage must be immediately observable through
 * file_read as a zero-filled range, even before delayed records are flushed. */
static enum theft_trial_res prop_sparse_clear_reads_as_zeroes(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    unsigned offset = case_offset(cc);
    file_write(&tb, offset, cc->len, (const char *)cc->data);
    clear_file(&node, offset, cc->len);

    unsigned char *out = malloc(cc->len);
    if (out == NULL) {
        cleanup_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&node, offset, cc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < cc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "sparse clear read non-zero byte=%u value=%u\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: clear_file with len == 0 is a no-op: it does not enqueue delayed
 * writes and does not change existing page contents. */
static enum theft_trial_res prop_zero_length_clear_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    unsigned region = case_region_bytes(cc);
    unsigned region_start = cc->start_page * PAGE_SIZE;
    if (!prefill_pages(&tb, cc->start_page, cc->page_count, cc->fill)) {
        cleanup_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    clear_file(&node, case_offset(cc), 0U);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_region(&node, region_start, region, out);

    int ok = alloc_buffer.size == 0U;
    if (!ok) fprintf(stderr, "zero-length clear enqueued %u records\n", alloc_buffer.size);

    for (unsigned i = 0; ok && i < region; i++) {
        if (out[i] != cc->fill) {
            fprintf(stderr, "zero-length clear changed byte=%u got=%u expected=%u\n",
                    i, out[i], cc->fill);
            ok = 0;
        }
    }

    free(out);
    cleanup_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &clear_case_info },
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

    printf("delay_alloc clear_file property-based tests:\n");
    failures += run_prop("clear_zeros_target_span", prop_clear_zeros_target_span, 300);
    failures += run_prop("sparse_clear_enqueues_touched_pages", prop_sparse_clear_enqueues_touched_pages, 300);
    failures += run_prop("sparse_clear_reads_as_zeroes", prop_sparse_clear_reads_as_zeroes, 300);
    failures += run_prop("zero_length_clear_is_noop", prop_zero_length_clear_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
