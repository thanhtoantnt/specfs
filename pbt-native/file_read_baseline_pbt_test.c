/*
 * Property-based tests for file_read() in
 * eval/extent/baseline/lowlevel_file.c
 *
 * Oracle: algebraic/reference-style byte model for index-table reads.
 * The properties check that allocated page bytes are copied exactly across
 * unaligned page boundaries, unallocated pages read as zeroes, reads do not
 * allocate or mutate the page table, and zero-length reads are a no-op.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_READ_BYTES 2048U
#define MAX_TEST_PAGES 4U
#define MAX_TEST_PAGE 16U

struct read_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_TEST_PAGES * PG_SIZE];
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

    rc->start_page = bounded_choice(t, MAX_TEST_PAGE);
    rc->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        rc->page_off = 0U;
        break;
    case 1:
        rc->page_off = 1U;
        break;
    case 2:
        rc->page_off = PG_SIZE - 1U;
        break;
    case 3:
        rc->page_off = PG_SIZE - 16U;
        break;
    case 4:
        rc->page_off = PG_SIZE / 2U;
        break;
    default:
        rc->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = rc->page_count * PG_SIZE - rc->page_off;
    unsigned max_len = span_bytes < MAX_READ_BYTES ? span_bytes : MAX_READ_BYTES;
    rc->len = 1U + bounded_choice(t, max_len);

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
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u}",
            rc->start_page, rc->page_count, rc->page_off, rc->len);
}

static struct theft_type_info read_case_info = {
    .alloc = read_case_alloc_cb,
    .free = read_case_free_cb,
    .hash = read_case_hash_cb,
    .print = read_case_print_cb,
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
                          unsigned page_count, const unsigned char *data)
{
    for (unsigned i = 0; i < page_count; i++) {
        tb->index[start_page + i] = malloc_page();
        if (tb->index[start_page + i] == NULL) return 0;
        memcpy(tb->index[start_page + i], data + (i * PG_SIZE), PG_SIZE);
    }
    return 1;
}

static unsigned case_offset(const struct read_case *rc)
{
    return rc->start_page * PG_SIZE + rc->page_off;
}

static unsigned count_allocated_pages(const struct indextb *tb)
{
    unsigned count = 0;
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        if (tb->index[i] != NULL) count++;
    }
    return count;
}

static enum theft_trial_res prop_allocated_read_matches_page_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(rc);

    if (!allocate_pages(&tb, rc->start_page, rc->page_count, rc->data)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char out[MAX_READ_BYTES];
    memset(out, 0xCC, sizeof(out));
    file_read(&tb, offset, rc->len, (char *)out);

    int ok = memcmp(out, rc->data + rc->page_off, rc->len) == 0;
    if (!ok) {
        fprintf(stderr, "allocated read mismatch: offset=%u len=%u\n", offset, rc->len);
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_read_zero_fills_gaps(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(rc);

    if (rc->start_page == 0U) {
        return THEFT_TRIAL_SKIP;
    }
    if (!allocate_pages(&tb, rc->start_page, rc->page_count, rc->data)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned read_start = offset - PG_SIZE;
    unsigned read_len = rc->len + (2U * PG_SIZE);
    unsigned char *out = malloc(read_len);
    if (out == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, read_len);
    file_read(&tb, read_start, read_len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < read_len; i++) {
        unsigned absolute = read_start + i;
        unsigned page = absolute / PG_SIZE;
        unsigned expected = 0U;
        if (page >= rc->start_page && page < rc->start_page + rc->page_count) {
            unsigned page_index = page - rc->start_page;
            unsigned page_offset = absolute % PG_SIZE;
            expected = rc->data[(page_index * PG_SIZE) + page_offset];
        }
        if (out[i] != expected) {
            fprintf(stderr,
                    "sparse read mismatch at byte=%u absolute=%u got=%u expected=%u\n",
                    i, absolute, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_unallocated_read_returns_zeroes_and_does_not_allocate(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(rc);
    unsigned before_count = count_allocated_pages(&tb);

    unsigned char out[MAX_READ_BYTES];
    memset(out, 0xCC, sizeof(out));
    file_read(&tb, offset, rc->len, (char *)out);

    int ok = count_allocated_pages(&tb) == before_count;
    for (unsigned i = 0; i < rc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "unallocated read returned non-zero at %u: %u\n", i, out[i]);
            ok = 0;
            break;
        }
    }
    if (!ok && count_allocated_pages(&tb) != before_count) {
        fprintf(stderr, "unallocated read changed allocation count from %u to %u\n",
                before_count, count_allocated_pages(&tb));
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_read_does_not_mutate_allocated_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(rc);

    if (!allocate_pages(&tb, rc->start_page, rc->page_count, rc->data)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char before[MAX_TEST_PAGES * PG_SIZE];
    memcpy(before, rc->data, sizeof(before));

    unsigned char out[MAX_READ_BYTES];
    file_read(&tb, offset, rc->len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < rc->page_count; i++) {
        if (memcmp(tb.index[rc->start_page + i], before + (i * PG_SIZE), PG_SIZE) != 0) {
            fprintf(stderr, "read mutated allocated page %u\n", rc->start_page + i);
            ok = 0;
            break;
        }
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_read_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(rc);
    unsigned char out[16];
    unsigned char before[16];

    if (!allocate_pages(&tb, rc->start_page, rc->page_count, rc->data)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(before));
    file_read(&tb, offset, 0U, (char *)out);

    int ok = memcmp(out, before, sizeof(out)) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length read modified destination\n");
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                 \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &read_case_info },                           \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("file_read baseline property-based tests:\n");
    RUN_PROP("allocated_read_matches_page_bytes", prop_allocated_read_matches_page_bytes, 300);
    RUN_PROP("sparse_read_zero_fills_gaps", prop_sparse_read_zero_fills_gaps, 300);
    RUN_PROP("unallocated_read_returns_zeroes_and_does_not_allocate", prop_unallocated_read_returns_zeroes_and_does_not_allocate, 200);
    RUN_PROP("read_does_not_mutate_allocated_pages", prop_read_does_not_mutate_allocated_pages, 200);
    RUN_PROP("zero_length_read_is_noop", prop_zero_length_read_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
