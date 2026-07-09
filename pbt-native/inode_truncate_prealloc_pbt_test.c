/*
 * Property-based tests for inode_truncate() in
 * eval/pre_alloc/optimization/inode_management.c
 *
 * Uses theft (C PBT framework): https://github.com/silentbicycle/theft
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inode_management.h"

static int g_clear_calls;
static unsigned g_clear_start;
static unsigned g_clear_len;

static void reset_observed(void)
{
    g_clear_calls = 0;
    g_clear_start = 0;
    g_clear_len = 0;
}

void clear_file(struct inode *node, unsigned start, unsigned len)
{
    (void)node;
    g_clear_calls++;
    g_clear_start = start;
    g_clear_len = len;
}

void file_allocate(struct indextb *tb, unsigned offset, unsigned len)
{
    (void)tb;
    (void)offset;
    (void)len;
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    (void)node;
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

unsigned int hash_func(char *name)
{
    (void)name;
    return 0;
}

struct truncate_case {
    unsigned old_size;
    unsigned new_size;
};

static enum theft_alloc_res truncate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct truncate_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    tc->old_size = (unsigned)theft_random_choice(t, 1U << 20);
    tc->new_size = (unsigned)theft_random_choice(t, 1U << 20);
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
    theft_hash_sink(&h, &tc->old_size, sizeof(tc->old_size));
    theft_hash_sink(&h, &tc->new_size, sizeof(tc->new_size));
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

static struct inode make_inode(unsigned size)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    return node;
}

static enum theft_trial_res prop_size_always_updates(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    struct inode node = make_inode(tc->old_size);

    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (node.size != tc->new_size) {
        fprintf(stderr, "size mismatch: old=%u new=%u got=%u\n",
                tc->old_size, tc->new_size, node.size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_shrink_clears_exact_tail(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size >= tc->old_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (g_clear_calls != 1 || g_clear_start != tc->new_size ||
        g_clear_len != tc->old_size - tc->new_size) {
        fprintf(stderr, "bad shrink clear: old=%u new=%u calls=%d start=%u len=%u\n",
                tc->old_size, tc->new_size, g_clear_calls, g_clear_start, g_clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_equal_size_does_not_clear(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    struct inode node = make_inode(tc->old_size);

    reset_observed();
    inode_truncate(&node, tc->old_size);

    if (g_clear_calls != 0 || node.size != tc->old_size) {
        fprintf(stderr, "equal truncate changed state: size=%u calls=%d\n",
                node.size, g_clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_grow_clears_new_region(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size <= tc->old_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (g_clear_calls != 1 || g_clear_start != tc->old_size ||
        g_clear_len != tc->new_size - tc->old_size) {
        fprintf(stderr, "bad grow clear: old=%u new=%u calls=%d start=%u len=%u\n",
                tc->old_size, tc->new_size, g_clear_calls, g_clear_start, g_clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_null_inode_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;

    reset_observed();
    inode_truncate(NULL, tc->new_size);

    if (g_clear_calls != 0) {
        fprintf(stderr, "null inode called clear_file %d times\n", g_clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
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
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);      \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("pre_alloc inode_truncate property-based tests:\n");
    RUN_PROP("size_always_updates", prop_size_always_updates, 500);
    RUN_PROP("shrink_clears_exact_tail", prop_shrink_clears_exact_tail, 500);
    RUN_PROP("equal_size_does_not_clear", prop_equal_size_does_not_clear, 200);
    RUN_PROP("grow_clears_new_region", prop_grow_clears_new_region, 500);
    RUN_PROP("null_inode_is_noop", prop_null_inode_is_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
