/*
 * Property-based tests for inode_truncate() in
 * eval/delay_alloc/optimization/inode_management.c
 *
 * Uses theft (C PBT framework): https://github.com/silentbicycle/theft
 *
 * The properties below pin the observed delay_alloc contract:
 * - truncation always updates node->size
 * - shrinking clears the dropped tail
 * - growing allocates and clears the newly exposed region
 * - no unrelated low-level write occurs
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

struct lowlevel_observation {
    unsigned allocate_calls;
    struct indextb *allocate_tb;
    unsigned allocate_offset;
    unsigned allocate_len;

    unsigned clear_calls;
    struct inode *clear_node;
    unsigned clear_offset;
    unsigned clear_len;

    unsigned write_calls;
};

static struct lowlevel_observation observed;

static void reset_observed(void)
{
    memset(&observed, 0, sizeof(observed));
}

void file_allocate(struct indextb *tb, unsigned offset, unsigned len)
{
    observed.allocate_calls++;
    observed.allocate_tb = tb;
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

void file_write(struct indextb *tb, unsigned offset, unsigned len, const char *data)
{
    (void)tb;
    (void)offset;
    (void)len;
    (void)data;
    observed.write_calls++;
}

void file_read(struct indextb *tb, unsigned offset, unsigned len, char *data)
{
    (void)tb;
    (void)offset;
    (void)len;
    (void)data;
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

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

unsigned int hash_func(char *name)
{
    (void)name;
    return 0;
}

struct truncate_case {
    unsigned old_size;
    unsigned new_size;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static unsigned boundary_near(struct theft *t, unsigned center, unsigned radius)
{
    unsigned delta = bounded_choice(t, radius);
    if (theft_random_choice(t, 2) == 0) {
        return center > delta ? center - delta : 0U;
    }
    if (UINT32_MAX - center < delta) return UINT32_MAX;
    return center + delta;
}

static enum theft_alloc_res truncate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct truncate_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8)) {
    case 0:
        tc->old_size = bounded_choice(t, 32U);
        tc->new_size = bounded_choice(t, 32U);
        break;
    case 1:
        tc->old_size = bounded_choice(t, PAGE_SIZE);
        tc->new_size = boundary_near(t, PAGE_SIZE, 128U);
        break;
    case 2:
        tc->old_size = boundary_near(t, PAGE_SIZE, 128U);
        tc->new_size = bounded_choice(t, PAGE_SIZE);
        break;
    case 3:
        tc->old_size = bounded_choice(t, MAX_FILE_SIZE);
        tc->new_size = bounded_choice(t, MAX_FILE_SIZE);
        break;
    case 4:
        tc->old_size = boundary_near(t, MAX_FILE_SIZE, 128U);
        tc->new_size = boundary_near(t, MAX_FILE_SIZE, 128U);
        break;
    case 5:
        tc->old_size = MAX_FILE_SIZE;
        tc->new_size = MAX_FILE_SIZE + 1U + bounded_choice(t, 1024U);
        break;
    case 6:
        tc->old_size = bounded_choice(t, MAX_FILE_SIZE);
        tc->new_size = MAX_FILE_SIZE + 1U + bounded_choice(t, 1024U);
        break;
    default:
        tc->old_size = bounded_choice(t, MAX_FILE_SIZE);
        tc->new_size = bounded_choice(t, MAX_FILE_SIZE + 1024U);
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
    const struct truncate_case *tc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&tc->old_size, sizeof(tc->old_size));
    theft_hash_sink(&h, (const uint8_t *)&tc->new_size, sizeof(tc->new_size));
    return theft_hash_done(&h);
}

static void truncate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    fprintf(f, "{old_size=%u, new_size=%u}", tc->old_size, tc->new_size);
}

static struct theft_type_info truncate_case_info = {
    .alloc = truncate_case_alloc_cb,
    .free = truncate_case_free_cb,
    .hash = truncate_case_hash_cb,
    .print = truncate_case_print_cb,
};

static struct inode make_inode(unsigned size, struct indextb *file)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    node.file = file;
    return node;
}

static enum theft_trial_res prop_size_always_updates(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    struct indextb file;
    memset(&file, 0, sizeof(file));
    struct inode node = make_inode(tc->old_size, &file);

    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (node.size != tc->new_size) {
        fprintf(stderr, "size mismatch: old=%u new=%u got=%u\n",
                tc->old_size, tc->new_size, node.size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_shrink_clears_tail(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size >= tc->old_size) return THEFT_TRIAL_SKIP;

    struct indextb file;
    memset(&file, 0, sizeof(file));
    struct inode node = make_inode(tc->old_size, &file);

    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (node.size != tc->new_size || observed.allocate_calls != 0 ||
        observed.clear_calls != 1 || observed.write_calls != 0 ||
        observed.clear_node != &node || observed.clear_offset != tc->new_size ||
        observed.clear_len != tc->old_size - tc->new_size) {
        fprintf(stderr,
                "bad shrink: old=%u new=%u size=%u alloc=%u clear=%u write=%u start=%u len=%u\n",
                tc->old_size, tc->new_size, node.size, observed.allocate_calls,
                observed.clear_calls, observed.write_calls, observed.clear_offset,
                observed.clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_grow_allocates_and_clears(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size <= tc->old_size) return THEFT_TRIAL_SKIP;

    struct indextb file;
    memset(&file, 0, sizeof(file));
    struct inode node = make_inode(tc->old_size, &file);

    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (node.size != tc->new_size || observed.allocate_calls != 1 ||
        observed.clear_calls != 1 || observed.write_calls != 0 ||
        observed.allocate_tb != &file || observed.allocate_offset != tc->old_size ||
        observed.allocate_len != tc->new_size - tc->old_size ||
        observed.clear_node != &node || observed.clear_offset != tc->old_size ||
        observed.clear_len != tc->new_size - tc->old_size) {
        fprintf(stderr,
                "bad grow: old=%u new=%u size=%u alloc=%u clear=%u write=%u alloc=(%u,%u) clear=(%u,%u)\n",
                tc->old_size, tc->new_size, node.size, observed.allocate_calls,
                observed.clear_calls, observed.write_calls, observed.allocate_offset,
                observed.allocate_len, observed.clear_offset, observed.clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_equal_size_is_storage_noop(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    struct indextb file;
    memset(&file, 0, sizeof(file));
    struct inode node = make_inode(tc->old_size, &file);

    reset_observed();
    inode_truncate(&node, tc->old_size);

    if (node.size != tc->old_size || observed.allocate_calls != 0 ||
        observed.clear_calls != 0 || observed.write_calls != 0) {
        fprintf(stderr, "equal truncate touched storage: size=%u alloc=%u clear=%u write=%u\n",
                node.size, observed.allocate_calls, observed.clear_calls, observed.write_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                               \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &truncate_case_info },                      \
            .trials = trials_,                                         \
            .seed = theft_seed_of_time(),                              \
        };                                                             \
        enum theft_run_res res = theft_run(&cfg);                      \
        printf("  [%s] %s\n",                                         \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);      \
        if (res != THEFT_RUN_PASS) failures++;                        \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("delay_alloc inode_truncate property-based tests:\n");
    RUN_PROP("size_always_updates", prop_size_always_updates, 500);
    RUN_PROP("shrink_clears_tail", prop_shrink_clears_tail, 500);
    RUN_PROP("grow_allocates_and_clears", prop_grow_allocates_and_clears, 500);
    RUN_PROP("equal_size_is_storage_noop", prop_equal_size_is_storage_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
