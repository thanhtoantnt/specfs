/*
 * Property-based tests for inode_truncate() in
 * eval/pre_alloc/baseline/inode_management.c
 *
 * Oracle: Algebraic invariant / negative contract over the public truncate
 * contract. A valid truncate must set node->size exactly to the requested
 * size. Shrinks must clear exactly the removed tail. Growth must clear the
 * newly exposed byte range so subsequent reads observe zeroes. Requests beyond
 * MAX_FILE_SIZE are invalid and must be no-ops. NULL inode input must be a
 * no-op.
 *
 * The low-level file operations are stubbed so these properties exercise the
 * real pre_alloc baseline inode_truncate symbol while observing only the
 * low-level calls that inode_truncate is responsible for issuing.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

struct truncate_observation {
    unsigned allocate_calls;
    struct inode *allocate_node;
    unsigned allocate_offset;
    unsigned allocate_len;

    unsigned clear_calls;
    struct inode *clear_node;
    unsigned clear_start;
    unsigned clear_len;

    unsigned read_calls;
    unsigned write_calls;
};

static struct truncate_observation observed;

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
    observed.clear_start = start;
    observed.clear_len = len;
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
    observed.read_calls++;
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
    observed.write_calls++;
}

struct read_ret *malloc_readret(void)
{
    return NULL;
}

char *malloc_buffer(unsigned len)
{
    (void)len;
    return NULL;
}

unsigned int hash_func(char *str)
{
    (void)str;
    return 0U;
}

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

struct truncate_case {
    unsigned old_size;
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
    return UINT32_MAX - center < delta ? UINT32_MAX : center + delta;
}

static enum theft_alloc_res truncate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct truncate_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 10U)) {
    case 0:
        tc->old_size = choice_inclusive(t, 32U);
        tc->requested_size = choice_inclusive(t, 32U);
        break;
    case 1:
        tc->old_size = 0U;
        tc->requested_size = choice_inclusive(t, PAGE_SIZE);
        break;
    case 2:
        tc->old_size = choice_inclusive(t, PAGE_SIZE);
        tc->requested_size = 0U;
        break;
    case 3:
        tc->old_size = near_boundary(t, PAGE_SIZE, 128U);
        tc->requested_size = near_boundary(t, PAGE_SIZE, 128U);
        break;
    case 4:
        tc->old_size = choice_inclusive(t, MAX_FILE_SIZE);
        tc->requested_size = choice_inclusive(t, MAX_FILE_SIZE);
        break;
    case 5:
        tc->old_size = near_boundary(t, MAX_FILE_SIZE, PAGE_SIZE);
        tc->requested_size = near_boundary(t, MAX_FILE_SIZE, PAGE_SIZE);
        break;
    case 6:
        tc->old_size = choice_inclusive(t, MAX_FILE_SIZE);
        tc->requested_size = MAX_FILE_SIZE + 1U + choice_inclusive(t, PAGE_SIZE);
        break;
    case 7:
        tc->old_size = MAX_FILE_SIZE;
        tc->requested_size = UINT32_MAX - choice_inclusive(t, PAGE_SIZE);
        break;
    case 8:
        tc->old_size = choice_inclusive(t, MAX_FILE_SIZE - 1U);
        tc->requested_size = tc->old_size + 1U +
                             choice_inclusive(t, MAX_FILE_SIZE - tc->old_size - 1U);
        break;
    default:
        tc->old_size = choice_inclusive(t, MAX_FILE_SIZE);
        tc->requested_size = choice_inclusive(t, MAX_FILE_SIZE + PAGE_SIZE);
        break;
    }

    if (tc->old_size > MAX_FILE_SIZE) tc->old_size = MAX_FILE_SIZE;

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
    const struct truncate_case *tc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&tc->old_size, sizeof(tc->old_size));
    theft_hash_sink(&h, (const uint8_t *)&tc->requested_size, sizeof(tc->requested_size));
    return theft_hash_done(&h);
}

static void truncate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    fprintf(f, "{old_size=%u, requested_size=%u}",
            tc->old_size, tc->requested_size);
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
    memset(&node, 0, sizeof(node));
    node.size = size;
    return node;
}

static enum theft_trial_res prop_valid_size_updates_exactly(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size > MAX_FILE_SIZE) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    if (node.size != tc->requested_size) {
        fprintf(stderr, "valid_size_updates_exactly failed: old=%u requested=%u got=%u\n",
                tc->old_size, tc->requested_size, node.size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_shrink_clears_removed_tail(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size >= tc->old_size || tc->requested_size > MAX_FILE_SIZE) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode node = make_inode(tc->old_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    int ok = observed.clear_calls == 1U && observed.clear_node == &node &&
             observed.clear_start == tc->requested_size &&
             observed.clear_len == tc->old_size - tc->requested_size &&
             observed.allocate_calls == 0U && observed.read_calls == 0U &&
             observed.write_calls == 0U && node.size == tc->requested_size;
    if (!ok) {
        fprintf(stderr,
                "shrink_clears_removed_tail failed: old=%u requested=%u size=%u "
                "clear=(calls=%u,start=%u,len=%u) allocs=%u reads=%u writes=%u\n",
                tc->old_size, tc->requested_size, node.size, observed.clear_calls,
                observed.clear_start, observed.clear_len, observed.allocate_calls,
                observed.read_calls, observed.write_calls);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_grow_clears_new_region(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size <= tc->old_size || tc->requested_size > MAX_FILE_SIZE) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode node = make_inode(tc->old_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    unsigned growth = tc->requested_size - tc->old_size;
    int ok = observed.clear_calls == 1U && observed.clear_node == &node &&
             observed.clear_start == tc->old_size && observed.clear_len == growth &&
             observed.read_calls == 0U && observed.write_calls == 0U &&
             node.size == tc->requested_size;
    if (!ok) {
        fprintf(stderr,
                "grow_clears_new_region failed: old=%u requested=%u size=%u "
                "clear=(calls=%u,start=%u,len=%u) allocs=%u reads=%u writes=%u\n",
                tc->old_size, tc->requested_size, node.size, observed.clear_calls,
                observed.clear_start, observed.clear_len, observed.allocate_calls,
                observed.read_calls, observed.write_calls);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_same_size_is_noop_except_size_assignment(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    struct inode node = make_inode(tc->old_size);
    reset_observed();

    inode_truncate(&node, tc->old_size);

    int ok = node.size == tc->old_size && observed.allocate_calls == 0U &&
             observed.clear_calls == 0U && observed.read_calls == 0U &&
             observed.write_calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "same_size_noop failed: old=%u size=%u allocs=%u clears=%u reads=%u writes=%u\n",
                tc->old_size, node.size, observed.allocate_calls,
                observed.clear_calls, observed.read_calls, observed.write_calls);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_out_of_range_size_is_rejected(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;
    if (tc->requested_size <= MAX_FILE_SIZE) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();

    inode_truncate(&node, tc->requested_size);

    int ok = node.size == tc->old_size && observed.allocate_calls == 0U &&
             observed.clear_calls == 0U && observed.read_calls == 0U &&
             observed.write_calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "out_of_range_size_is_rejected failed: old=%u requested=%u size=%u "
                "allocs=%u clears=%u reads=%u writes=%u\n",
                tc->old_size, tc->requested_size, node.size, observed.allocate_calls,
                observed.clear_calls, observed.read_calls, observed.write_calls);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_inode_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct truncate_case *tc = arg1;

    reset_observed();
    inode_truncate(NULL, tc->requested_size);

    int ok = observed.allocate_calls == 0U && observed.clear_calls == 0U &&
             observed.read_calls == 0U && observed.write_calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "null_inode_is_noop failed: requested=%u allocs=%u clears=%u reads=%u writes=%u\n",
                tc->requested_size, observed.allocate_calls, observed.clear_calls,
                observed.read_calls, observed.write_calls);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &truncate_case_info },                      \
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

    printf("pre_alloc baseline inode_truncate property-based tests:\n");
    RUN_PROP("valid_size_updates_exactly", prop_valid_size_updates_exactly, 500);
    RUN_PROP("shrink_clears_removed_tail", prop_shrink_clears_removed_tail, 500);
    RUN_PROP("grow_clears_new_region", prop_grow_clears_new_region, 500);
    RUN_PROP("same_size_is_noop_except_size_assignment", prop_same_size_is_noop_except_size_assignment, 500);
    RUN_PROP("out_of_range_size_is_rejected", prop_out_of_range_size_is_rejected, 500);
    RUN_PROP("null_inode_is_noop", prop_null_inode_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
