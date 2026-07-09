/*
 * Property-based tests for inode_write() in
 * eval/inline_data/optimization/inode_management.c
 *
 * These tests isolate inode_write with stubbed low-level file operations so the
 * observable contract is the returned byte count, inode size, allocation/clear
 * range, and final write call. The generator is skewed toward MAX_FILE_SIZE and
 * INLINE_DATA_SIZE boundaries to exercise inline-data transition cases.
 */
#include <theft.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

#define MAX_BUF_LEN 512

struct lowlevel_observation {
    unsigned allocate_calls;
    struct inode *allocate_node;
    unsigned allocate_offset;
    unsigned allocate_len;

    unsigned clear_calls;
    struct inode *clear_node;
    unsigned clear_offset;
    unsigned clear_len;

    unsigned write_calls;
    struct inode *write_node;
    unsigned write_offset;
    unsigned write_len;
    const char *write_data;
};

static struct lowlevel_observation observed;

static void reset_observed(void)
{
    memset(&observed, 0, sizeof(observed));
}

void file_allocate(struct inode *node, unsigned offset, unsigned len)
{
    observed.allocate_calls++;
    observed.allocate_node = node;
    observed.allocate_offset = offset;
    observed.allocate_len = len;
}

void clear_file(struct inode *node, unsigned start, unsigned len)
{
    observed.clear_calls++;
    observed.clear_node = node;
    observed.clear_offset = start;
    observed.clear_len = len;
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    observed.write_calls++;
    observed.write_node = node;
    observed.write_offset = offset;
    observed.write_len = len;
    observed.write_data = data;
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
}

struct read_ret *malloc_readret(void)
{
    return malloc(sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    return malloc(len == 0 ? 1 : len);
}

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

unsigned int hash_func(char *str)
{
    (void)str;
    return 0;
}

struct write_case {
    unsigned initial_size;
    unsigned offset;
    unsigned len;
    char buffer[MAX_BUF_LEN];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static unsigned boundary_near(struct theft *t, unsigned center, unsigned radius)
{
    unsigned delta = bounded_choice(t, radius);
    if (theft_random_choice(t, 2) == 0) {
        return center > delta ? center - delta : 0;
    }
    if (UINT32_MAX - center < delta) return UINT32_MAX;
    return center + delta;
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 6)) {
    case 0:
        wc->initial_size = bounded_choice(t, INLINE_DATA_SIZE);
        break;
    case 1:
        wc->initial_size = boundary_near(t, INLINE_DATA_SIZE, MAX_BUF_LEN);
        if (wc->initial_size > MAX_FILE_SIZE) wc->initial_size = MAX_FILE_SIZE;
        break;
    default:
        wc->initial_size = bounded_choice(t, MAX_FILE_SIZE);
        break;
    }

    switch (theft_random_choice(t, 10)) {
    case 0:
        wc->offset = bounded_choice(t, INLINE_DATA_SIZE);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 1:
        wc->offset = boundary_near(t, INLINE_DATA_SIZE, MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 2:
        wc->offset = INLINE_DATA_SIZE > MAX_BUF_LEN
            ? INLINE_DATA_SIZE - bounded_choice(t, MAX_BUF_LEN)
            : 0;
        wc->len = MAX_BUF_LEN + bounded_choice(t, MAX_BUF_LEN);
        break;
    case 3:
        wc->offset = bounded_choice(t, MAX_FILE_SIZE);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 4:
        wc->offset = MAX_FILE_SIZE;
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 5:
        wc->offset = bounded_choice(t, MAX_FILE_SIZE);
        wc->len = MAX_FILE_SIZE - wc->offset + bounded_choice(t, MAX_BUF_LEN);
        break;
    case 6:
        wc->offset = MAX_FILE_SIZE + 1U + bounded_choice(t, MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 7:
        wc->offset = UINT32_MAX - bounded_choice(t, MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    default:
        wc->offset = bounded_choice(t, MAX_FILE_SIZE + MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    }

    for (unsigned i = 0; i < MAX_BUF_LEN; i++) {
        wc->buffer[i] = (char)theft_random_choice(t, 256);
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
    fprintf(f, "{initial_size=%u, offset=%u, len=%u}",
            wc->initial_size, wc->offset, wc->len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static unsigned expected_actual_len(unsigned len, unsigned offset)
{
    if (offset >= MAX_FILE_SIZE) return 0;
    unsigned remaining = MAX_FILE_SIZE - offset;
    return len > remaining ? remaining : len;
}

static unsigned expected_size_after_write(unsigned initial_size, unsigned offset, unsigned actual)
{
    if (actual == 0) return initial_size;
    unsigned written_end = offset + actual;
    return written_end > initial_size ? written_end : initial_size;
}

static struct inode make_inode(unsigned size)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    return node;
}

static bool growth_matches(unsigned initial_size, unsigned expected_size)
{
    if (expected_size > initial_size) {
        unsigned grow_len = expected_size - initial_size;
        return observed.allocate_calls == 1 && observed.allocate_node == observed.clear_node &&
               observed.allocate_node != NULL && observed.allocate_offset == initial_size &&
               observed.allocate_len == grow_len && observed.clear_calls == 1 &&
               observed.clear_offset == initial_size && observed.clear_len == grow_len;
    }

    return observed.allocate_calls == 0 && observed.clear_calls == 0;
}

static enum theft_trial_res prop_return_is_clamped(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected = expected_actual_len(wc->len, wc->offset);

    if (actual != expected) {
        fprintf(stderr, "return_is_clamped failed: got %u expected %u\n", actual, expected);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_size_updates_to_written_end(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected_size = expected_size_after_write(wc->initial_size, wc->offset, actual);

    if (node.size != expected_size) {
        fprintf(stderr, "size_update failed: got %u expected %u actual=%u\n",
                node.size, expected_size, actual);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_growth_is_allocated_and_cleared(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected_size = expected_size_after_write(wc->initial_size, wc->offset, actual);

    if (!growth_matches(wc->initial_size, expected_size)) {
        fprintf(stderr,
                "growth mismatch: alloc_calls=%u clear_calls=%u alloc=(%u,%u) clear=(%u,%u) initial=%u expected_size=%u\n",
                observed.allocate_calls, observed.clear_calls,
                observed.allocate_offset, observed.allocate_len,
                observed.clear_offset, observed.clear_len,
                wc->initial_size, expected_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_file_write_matches_effective_write(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    if (actual == 0) {
        if (observed.write_calls != 0) {
            fprintf(stderr, "zero write called file_write %u times\n", observed.write_calls);
            return THEFT_TRIAL_FAIL;
        }
        return THEFT_TRIAL_PASS;
    }

    if (observed.write_calls != 1 || observed.write_node != &node ||
        observed.write_offset != wc->offset || observed.write_len != actual ||
        observed.write_data != wc->buffer) {
        fprintf(stderr, "file_write mismatch: calls=%u offset=%u len=%u actual=%u\n",
                observed.write_calls, observed.write_offset, observed.write_len, actual);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_out_of_range_offsets_are_noops(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    if (wc->offset < MAX_FILE_SIZE) return THEFT_TRIAL_PASS;

    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    if (actual != 0 || node.size != wc->initial_size || observed.allocate_calls != 0 ||
        observed.clear_calls != 0 || observed.write_calls != 0) {
        fprintf(stderr,
                "out_of_range_noop failed: actual=%u size=%u initial=%u alloc=%u clear=%u write=%u\n",
                actual, node.size, wc->initial_size, observed.allocate_calls,
                observed.clear_calls, observed.write_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &write_case_info },                          \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("inline_data inode_write property-based tests:\n");
    RUN_PROP("return_is_clamped", prop_return_is_clamped, 500);
    RUN_PROP("size_updates_to_written_end", prop_size_updates_to_written_end, 500);
    RUN_PROP("growth_is_allocated_and_cleared", prop_growth_is_allocated_and_cleared, 500);
    RUN_PROP("file_write_matches_effective_write", prop_file_write_matches_effective_write, 500);
    RUN_PROP("out_of_range_offsets_are_noops", prop_out_of_range_offsets_are_noops, 500);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
