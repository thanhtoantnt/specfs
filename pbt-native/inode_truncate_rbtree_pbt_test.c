/*
 * Property-based tests for inode_truncate() in
 * eval/rbtree/optimization/inode_management.c
 *
 * Uses theft (C PBT framework): https://github.com/silentbicycle/theft
 *
 * Coq spec (inode_truncate.spec):
 *   Case 1: size < old_size → clear [size, old_size), update node->size.
 *   Case 2: size >= old_size → allocate [old_size, size), clear it, update size.
 *
 * The optimization's Case 2 only sets node->size = size WITHOUT calling
 * clear_file (or file_allocate). This test pins the SPEC contract (not the
 * buggy implementation): growth must call clear_file to zero the new region.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inode_management.h"

/* ---- Observation stubs for lowlevel_file deps ---- */

static int g_clear_calls;
static unsigned g_clear_start;
static unsigned g_clear_len;

static void reset_observed(void)
{
    g_clear_calls = 0;
    g_clear_start = 0;
    g_clear_len = 0;
}

/* Stub: clear_file — observe calls instead of doing real I/O */
void clear_file(struct inode *node, unsigned start, unsigned len)
{
    (void)node;
    g_clear_calls++;
    g_clear_start = start;
    g_clear_len = len;
}

/* Stubs for other external deps in inode_management.c */
void file_allocate(struct indextb *tb, unsigned offset, unsigned len)
{
    (void)tb; (void)offset; (void)len;
}
void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node; (void)offset; (void)len; (void)data;
}
void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    (void)node; (void)offset; (void)len; (void)data;
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

/* ---- Input generator ---- */

struct truncate_case {
    unsigned old_size;
    unsigned new_size;
};

static enum theft_alloc_res
truncate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct truncate_case *tc = malloc(sizeof(*tc));
    if (!tc) return THEFT_ALLOC_ERROR;
    /* Range [0, 2^20) for both — exercises grow, shrink, and equal */
    tc->old_size = (unsigned)theft_random_choice(t, 1 << 20);
    tc->new_size = (unsigned)theft_random_choice(t, 1 << 20);
    *instance = tc;
    return THEFT_ALLOC_OK;
}

static void truncate_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash
truncate_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, &tc->old_size, sizeof(tc->old_size));
    theft_hash_sink(&h, &tc->new_size, sizeof(tc->new_size));
    return theft_hash_done(&h);
}

static void
truncate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    fprintf(f, "{old_size=%u, new_size=%u}", tc->old_size, tc->new_size);
}

static struct theft_type_info truncate_case_info = {
    .alloc  = truncate_case_alloc_cb,
    .free   = truncate_case_free_cb,
    .hash   = truncate_case_hash_cb,
    .print  = truncate_case_print_cb,
};

/* ---- Properties ---- */

static struct inode make_inode(unsigned size)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    return node;
}

/* Property 1: size always updates to the requested value */
static enum theft_trial_res
prop_size_always_updates(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);
    if (node.size != tc->new_size) {
        fprintf(stderr, "FAIL size: old=%u new=%u got size=%u\n",
                tc->old_size, tc->new_size, node.size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Property 2: shrink (new < old) calls clear_file exactly once on [new, old-new) */
static enum theft_trial_res
prop_shrink_clears_tail(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size >= tc->old_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (g_clear_calls != 1) {
        fprintf(stderr, "FAIL shrink clear_calls: old=%u new=%u got %d calls (expect 1)\n",
                tc->old_size, tc->new_size, g_clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    if (g_clear_start != tc->new_size || g_clear_len != tc->old_size - tc->new_size) {
        fprintf(stderr, "FAIL shrink clear_args: start=%u len=%u (expect %u, %u)\n",
                g_clear_start, g_clear_len, tc->new_size, tc->old_size - tc->new_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/*
 * Property 3 (SPEC CONTRACT — the bug): growth (new >= old) MUST call
 * clear_file to zero the newly allocated region [old, new-old).
 *
 * The Coq spec says:
 *   Case 2: If size >= current size, allocate additional space and clear it.
 *
 * The optimization's implementation only sets node->size = size without
 * calling clear_file or file_allocate. This property pins the SPEC, not the
 * buggy implementation.
 */
static enum theft_trial_res
prop_grow_clears_new_region(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size <= tc->old_size) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (g_clear_calls != 1) {
        fprintf(stderr, "FAIL grow clear_calls: old=%u new=%u got %d calls (expect 1)\n",
                tc->old_size, tc->new_size, g_clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    if (g_clear_start != tc->old_size || g_clear_len != tc->new_size - tc->old_size) {
        fprintf(stderr, "FAIL grow clear_args: start=%u len=%u (expect %u, %u)\n",
                g_clear_start, g_clear_len, tc->old_size, tc->new_size - tc->old_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Property 4: null inode is a safe no-op */
static enum theft_trial_res
prop_null_is_safe(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    reset_observed();
    inode_truncate(NULL, tc->new_size);
    if (g_clear_calls != 0) {
        fprintf(stderr, "FAIL null: clear_file called %d times\n", g_clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ---- Runner ---- */

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &truncate_case_info },                       \
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

    printf("inode_truncate property-based tests:\n");
    RUN_PROP("size_always_updates",    prop_size_always_updates,    500);
    RUN_PROP("shrink_clears_tail",     prop_shrink_clears_tail,     500);
    RUN_PROP("grow_clears_new_region", prop_grow_clears_new_region, 500);
    RUN_PROP("null_is_safe",           prop_null_is_safe,           200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
