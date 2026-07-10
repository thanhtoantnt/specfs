/*
 * Property-based tests for file_read() in
 * eval/inline_data/baseline/lowlevel_file.c
 *
 * Oracle: reference byte model for sparse page-table reads. The properties
 * check exact byte projection across page boundaries, zero-fill behavior for
 * unallocated pages, immutability of storage/allocation state, zero-length
 * reads, and the generated implementation's data_block_count side effect.
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
#define MAX_COUNTER_FILES 3U

struct read_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned present_mask;
    unsigned char data[MAX_TEST_PAGES][PG_SIZE];
};

struct counter cnt = { .size = 0 };

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *read_case = malloc(sizeof(*read_case));
    if (read_case == NULL) return THEFT_ALLOC_ERROR;

    read_case->start_page = bounded_choice(t, MAX_TEST_PAGE);
    read_case->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        read_case->page_off = 0U;
        break;
    case 1:
        read_case->page_off = 1U;
        break;
    case 2:
        read_case->page_off = PG_SIZE - 1U;
        break;
    case 3:
        read_case->page_off = PG_SIZE - 16U;
        break;
    case 4:
        read_case->page_off = PG_SIZE / 2U;
        break;
    default:
        read_case->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = read_case->page_count * PG_SIZE - read_case->page_off;
    unsigned max_len = span_bytes < MAX_READ_BYTES ? span_bytes : MAX_READ_BYTES;
    read_case->len = 1U + bounded_choice(t, max_len);

    read_case->present_mask = (unsigned)theft_random_choice(t, (uint64_t)(1U << read_case->page_count));
    for (unsigned page_idx = 0; page_idx < MAX_TEST_PAGES; page_idx++) {
        for (unsigned byte_idx = 0; byte_idx < PG_SIZE; byte_idx++) {
            read_case->data[page_idx][byte_idx] = (unsigned char)theft_random_choice(t, 256U);
        }
    }

    *instance = read_case;
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

static void read_case_print_cb(FILE *file, const void *instance, void *env)
{
    (void)env;
    const struct read_case *read_case = instance;
    fprintf(file, "{start_page=%u, page_count=%u, page_off=%u, len=%u, present_mask=0x%x}",
            read_case->start_page, read_case->page_count, read_case->page_off,
            read_case->len, read_case->present_mask);
}

static struct theft_type_info read_case_info = {
    .alloc = read_case_alloc_cb,
    .free = read_case_free_cb,
    .hash = read_case_hash_cb,
    .print = read_case_print_cb,
};

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

static int populate_sparse_pages(struct indextb *tb, const struct read_case *read_case)
{
    for (unsigned page_idx = 0; page_idx < read_case->page_count; page_idx++) {
        if (((read_case->present_mask >> page_idx) & 1U) == 0U) continue;

        unsigned page = read_case->start_page + page_idx;
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
        memcpy(tb->index[page], read_case->data[page_idx], PG_SIZE);
    }
    return 1;
}

static unsigned case_offset(const struct read_case *read_case)
{
    return read_case->start_page * PG_SIZE + read_case->page_off;
}

static unsigned count_allocated_pages(const struct indextb *tb)
{
    unsigned count = 0;
    for (unsigned page_idx = 0; page_idx < INDEXTB_NUM; page_idx++) {
        if (tb->index[page_idx] != NULL) count++;
    }
    return count;
}

static unsigned char model_byte(const struct read_case *read_case, unsigned absolute)
{
    unsigned page = absolute / PG_SIZE;
    unsigned page_off = absolute % PG_SIZE;

    if (page < read_case->start_page ||
        page >= read_case->start_page + read_case->page_count) {
        return 0U;
    }

    unsigned page_idx = page - read_case->start_page;
    if (((read_case->present_mask >> page_idx) & 1U) == 0U) return 0U;
    return read_case->data[page_idx][page_off];
}

static void fill_expected(const struct read_case *read_case, unsigned offset,
                          unsigned len, unsigned char *expected)
{
    for (unsigned byte_idx = 0; byte_idx < len; byte_idx++) {
        expected[byte_idx] = model_byte(read_case, offset + byte_idx);
    }
}

static int compare_buffers(const unsigned char *actual, const unsigned char *expected,
                           unsigned len, const char *label,
                           const struct read_case *read_case)
{
    for (unsigned byte_idx = 0; byte_idx < len; byte_idx++) {
        if (actual[byte_idx] != expected[byte_idx]) {
            fprintf(stderr,
                    "%s mismatch byte=%u got=%u expected=%u offset=%u len=%u mask=0x%x\n",
                    label, byte_idx, actual[byte_idx], expected[byte_idx],
                    case_offset(read_case), read_case->len, read_case->present_mask);
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_sparse_read_matches_reference_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *read_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(read_case);
    unsigned before_count;
    unsigned after_count;
    unsigned char *actual = malloc(read_case->len);
    unsigned char *expected = malloc(read_case->len);

    if (actual == NULL || expected == NULL) {
        free(actual);
        free(expected);
        return THEFT_TRIAL_ERROR;
    }
    if (!populate_sparse_pages(&tb, read_case)) {
        free(actual);
        free(expected);
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_expected(read_case, offset, read_case->len, expected);
    before_count = count_allocated_pages(&tb);
    memset(actual, 0xCC, read_case->len);
    file_read(&tb, offset, read_case->len, (char *)actual);
    after_count = count_allocated_pages(&tb);

    int ok = compare_buffers(actual, expected, read_case->len, "sparse_read", read_case) &&
             before_count == after_count;
    if (!ok && before_count != after_count) {
        fprintf(stderr, "read changed allocation count from %u to %u\n", before_count, after_count);
    }

    free(actual);
    free(expected);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_unallocated_read_returns_zeroes_and_does_not_allocate(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *read_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(read_case);
    unsigned char out[MAX_READ_BYTES];
    unsigned before_count = count_allocated_pages(&tb);

    memset(out, 0xCC, sizeof(out));
    file_read(&tb, offset, read_case->len, (char *)out);

    int ok = count_allocated_pages(&tb) == before_count;
    for (unsigned byte_idx = 0; byte_idx < read_case->len; byte_idx++) {
        if (out[byte_idx] != 0U) {
            fprintf(stderr, "unallocated read byte=%u got=%u expected=0\n", byte_idx, out[byte_idx]);
            ok = 0;
            break;
        }
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_read_does_not_mutate_allocated_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *read_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(read_case);
    unsigned char before[MAX_TEST_PAGES][PG_SIZE];
    unsigned char out[MAX_READ_BYTES];

    if (!populate_sparse_pages(&tb, read_case)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memcpy(before, read_case->data, sizeof(before));

    file_read(&tb, offset, read_case->len, (char *)out);

    int ok = 1;
    for (unsigned page_idx = 0; page_idx < read_case->page_count; page_idx++) {
        if (((read_case->present_mask >> page_idx) & 1U) == 0U) continue;
        unsigned page = read_case->start_page + page_idx;
        if (memcmp(tb.index[page], before[page_idx], PG_SIZE) != 0) {
            fprintf(stderr, "read mutated allocated page=%u\n", page);
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
    const struct read_case *read_case = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(read_case);
    unsigned char out[16];
    unsigned char before[16];
    unsigned before_count;

    if (!populate_sparse_pages(&tb, read_case)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(before));
    before_count = count_allocated_pages(&tb);
    file_read(&tb, offset, 0U, (char *)out);

    int ok = memcmp(out, before, sizeof(out)) == 0 &&
             count_allocated_pages(&tb) == before_count;
    if (!ok) fprintf(stderr, "zero-length read modified destination or allocation state\n");

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_data_block_count_matches_global_counter(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *read_case = arg1;
    struct indextb tables[MAX_COUNTER_FILES];
    struct inode nodes[MAX_COUNTER_FILES];
    unsigned char out[MAX_READ_BYTES];
    unsigned expected_blocks = 0;
    unsigned actual_blocks = 0;
    int scanned;

    for (unsigned file_idx = 0; file_idx < MAX_COUNTER_FILES; file_idx++) {
        tables[file_idx] = make_table();
        memset(&nodes[file_idx], 0, sizeof(nodes[file_idx]));
        nodes[file_idx].file = &tables[file_idx];
    }

    for (unsigned file_idx = 0; file_idx < MAX_COUNTER_FILES; file_idx++) {
        for (unsigned page_idx = 0; page_idx < read_case->page_count; page_idx++) {
            if (((read_case->present_mask >> page_idx) & 1U) == 0U) continue;
            unsigned page = read_case->start_page + page_idx + file_idx;
            tables[file_idx].index[page] = malloc_page();
            if (tables[file_idx].index[page] == NULL) {
                for (unsigned cleanup_idx = 0; cleanup_idx < MAX_COUNTER_FILES; cleanup_idx++) {
                    destroy_table(&tables[cleanup_idx]);
                }
                return THEFT_TRIAL_ERROR;
            }
            expected_blocks++;
        }
    }

    cnt.size = MAX_COUNTER_FILES;
    for (unsigned file_idx = 0; file_idx < MAX_COUNTER_FILES; file_idx++) {
        cnt.files[file_idx] = &nodes[file_idx];
    }

    remove("data_block_count");
    memset(out, 0xCC, sizeof(out));
    file_read(&tables[0], case_offset(read_case), read_case->len, (char *)out);

    FILE *file = fopen("data_block_count", "r");
    scanned = file != NULL ? fscanf(file, "%u", &actual_blocks) : 0;
    if (file != NULL) fclose(file);

    int ok = scanned == 1 && actual_blocks == expected_blocks;
    if (!ok) {
        fprintf(stderr, "data_block_count got=%u expected=%u scanned=%d\n",
                actual_blocks, expected_blocks, scanned);
    }

    remove("data_block_count");
    cnt.size = 0;
    for (unsigned file_idx = 0; file_idx < MAX_COUNTER_FILES; file_idx++) {
        cnt.files[file_idx] = NULL;
        destroy_table(&tables[file_idx]);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &read_case_info },                          \
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

    printf("inline_data baseline file_read property-based tests:\n");
    RUN_PROP("sparse_read_matches_reference_model", prop_sparse_read_matches_reference_model, 300);
    RUN_PROP("unallocated_read_returns_zeroes_and_does_not_allocate", prop_unallocated_read_returns_zeroes_and_does_not_allocate, 200);
    RUN_PROP("read_does_not_mutate_allocated_pages", prop_read_does_not_mutate_allocated_pages, 200);
    RUN_PROP("zero_length_read_is_noop", prop_zero_length_read_is_noop, 100);
    RUN_PROP("data_block_count_matches_global_counter", prop_data_block_count_matches_global_counter, 100);

    remove("data_block_count");
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
