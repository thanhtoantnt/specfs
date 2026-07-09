/*
 * Property-based tests for inode_truncate() in
 * eval/loc/gen/inode/inode_truncate.c
 *
 * Oracle: Algebraic invariant over the public truncate contract. Stronger
 * state-machine testing is unnecessary because inode_truncate is a single-call
 * mutation whose observable state is node->size plus the low-level allocation
 * and clear calls. The low-level file operations are stubbed so these
 * properties exercise the real generated inode_truncate symbol while observing
 * the requested storage contract precisely.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inode.h"

struct file_allocate_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
};

struct file_clear_observation {
    unsigned calls;
    struct inode *node;
    unsigned start;
    unsigned len;
};

static struct file_allocate_observation observed_allocate;
static struct file_clear_observation observed_clear;

static void reset_observed(void)
{
    memset(&observed_allocate, 0, sizeof(observed_allocate));
    memset(&observed_clear, 0, sizeof(observed_clear));
}

void file_allocate(struct inode *node, unsigned offset, unsigned len)
{
    observed_allocate.calls++;
    observed_allocate.node = node;
    observed_allocate.offset = offset;
    observed_allocate.len = len;
}

void file_clear(struct inode *node, unsigned start, unsigned len)
{
    observed_clear.calls++;
    observed_clear.node = node;
    observed_clear.start = start;
    observed_clear.len = len;
}

struct truncate_case {
    unsigned initial_size;
    unsigned requested_size;
};

static unsigned choice_inclusive(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static unsigned near_boundary(struct theft *t, unsigned center, unsigned radius)
{
    unsigned delta = choice_inclusive(t, radius);
    if (theft_random_choice(t, 2U) == 0U) {
        return center > delta ? center - delta : 0U;
    }
    uint64_t value = (uint64_t)center + (uint64_t)delta;
    return value > MAX_FILE_SIZE ? MAX_FILE_SIZE : (unsigned)value;
}

static enum theft_alloc_res truncate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct truncate_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    tc->initial_size = choice_inclusive(t, MAX_FILE_SIZE);

    switch (theft_random_choice(t, 8U)) {
    case 0:
        tc->requested_size = 0U;
        break;
    case 1:
        tc->requested_size = MAX_FILE_SIZE;
        break;
    case 2:
        tc->requested_size = tc->initial_size;
        break;
    case 3:
        tc->requested_size = near_boundary(t, tc->initial_size, 16U);
        break;
    case 4:
        tc->requested_size = choice_inclusive(t, tc->initial_size);
        break;
    case 5:
        tc->requested_size = tc->initial_size + choice_inclusive(t, MAX_FILE_SIZE - tc->initial_size);
        break;
    default:
        tc->requested_size = choice_inclusive(t, MAX_FILE_SIZE);
        break;
    }

    *instance = tc;
    return THEFT_ALLOC_OK;
}

static void truncate_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash truncate_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct truncate_case));
}

static void truncate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    fprintf(f, "{initial_size=%u, requested_size=%u}",
            tc->initial_size, tc->requested_size);
}

static struct theft_type_info truncate_case_info = {
    .alloc = truncate_case_alloc_cb,
    .free = truncate_case_free_cb,
    .hash = truncate_case_hash_cb,
    .print = truncate_case_print_cb,
};

static struct inode make_inode(unsigned size)
{
    struct inode node;
    static struct indextb tb;
    memset(&node, 0, sizeof(node));
    memset(&tb, 0, sizeof(tb));
    node.size = size;
    node.file = &tb;
    return node;
}

/* Oracle: every valid truncation sets inode size exactly to the requested size. */
static enum theft_trial_res prop_size_updates_exactly(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    struct inode node = make_inode(tc->initial_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    if (node.size != tc->requested_size) {
        fprintf(stderr, "size_updates_exactly failed: got=%u expected=%u initial=%u\n",
                node.size, tc->requested_size, tc->initial_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Oracle: shrinking clears exactly the removed tail and does not allocate. */
static enum theft_trial_res prop_shrink_clears_removed_tail(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size >= tc->initial_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->initial_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    unsigned expected_len = tc->initial_size - tc->requested_size;
    int ok = node.size == tc->requested_size &&
             observed_allocate.calls == 0U &&
             observed_clear.calls == 1U &&
             observed_clear.node == &node &&
             observed_clear.start == tc->requested_size &&
             observed_clear.len == expected_len;

    if (!ok) {
        fprintf(stderr,
                "shrink mismatch: size=%u expected_size=%u allocs=%u clears=%u clear_start=%u clear_len=%u expected_start=%u expected_len=%u initial=%u\n",
                node.size, tc->requested_size, observed_allocate.calls,
                observed_clear.calls, observed_clear.start, observed_clear.len,
                tc->requested_size, expected_len, tc->initial_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Oracle: growing allocates and clears exactly the new region from the old EOF. */
static enum theft_trial_res prop_grow_allocates_and_clears_new_region(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size <= tc->initial_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->initial_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    unsigned expected_len = tc->requested_size - tc->initial_size;
    int ok = node.size == tc->requested_size &&
             observed_allocate.calls == 1U &&
             observed_clear.calls == 1U &&
             observed_allocate.node == &node &&
             observed_clear.node == &node &&
             observed_allocate.offset == tc->initial_size &&
             observed_clear.start == tc->initial_size &&
             observed_allocate.len == expected_len &&
             observed_clear.len == expected_len;

    if (!ok) {
        fprintf(stderr,
                "grow mismatch: size=%u expected_size=%u allocs=%u alloc_off=%u alloc_len=%u clears=%u clear_start=%u clear_len=%u initial=%u expected_len=%u\n",
                node.size, tc->requested_size, observed_allocate.calls,
                observed_allocate.offset, observed_allocate.len,
                observed_clear.calls, observed_clear.start, observed_clear.len,
                tc->initial_size, expected_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Oracle: truncating to the current size is a storage no-op. */
static enum theft_trial_res prop_equal_size_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size != tc->initial_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->initial_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    int ok = node.size == tc->initial_size &&
             observed_allocate.calls == 0U &&
             observed_clear.calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "equal-size noop failed: size=%u initial=%u allocs=%u clears=%u\n",
                node.size, tc->initial_size, observed_allocate.calls,
                observed_clear.calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &truncate_case_info },
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

    printf("loc inode_truncate property-based tests:\n");
    failures += run_prop("size_updates_exactly", prop_size_updates_exactly, 500);
    failures += run_prop("shrink_clears_removed_tail", prop_shrink_clears_removed_tail, 500);
    failures += run_prop("grow_allocates_and_clears_new_region", prop_grow_allocates_and_clears_new_region, 500);
    failures += run_prop("equal_size_is_noop", prop_equal_size_is_noop, 500);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
