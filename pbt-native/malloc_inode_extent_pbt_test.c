/*
 * Property-based tests for malloc_inode() in
 * eval/extent/optimization/util.c
 *
 * Oracle: Algebraic - constructor invariant / layout contract
 * Stronger considered:
 *   - State Machine: rejected, malloc_inode is a single-shot constructor with no lifecycle transitions.
 *   - Differential: rejected, there is no independent extent constructor to compare against.
 * Weaker available: Reference, Crash-Only
 */
#include <limits.h>
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "util.h"

struct inode_case {
    int mode;
    unsigned maj;
    unsigned min;
};

static unsigned draw_edge_u32(struct theft *t)
{
    switch (theft_random_choice(t, 6U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return 42U;
    case 3:
        return UINT_MAX;
    default:
        return (unsigned)theft_random_choice(t, 100000U);
    }
}

static int draw_mode(struct theft *t)
{
    switch (theft_random_choice(t, 10U)) {
    case 0:
        return DIR_MODE;
    case 1:
        return FILE_MODE;
    case 2:
        return CHR_MODE;
    case 3:
        return BLK_MODE;
    case 4:
        return SOCK_MODE;
    case 5:
        return FIFO_MODE;
    case 6:
        return 0;
    case 7:
        return -1;
    case 8:
        return INT_MAX;
    default:
        return INT_MIN;
    }
}

static enum theft_alloc_res inode_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct inode_case *ic = calloc(1U, sizeof(*ic));
    if (ic == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    ic->mode = draw_mode(t);
    ic->maj = draw_edge_u32(t);
    ic->min = draw_edge_u32(t);

    *instance = ic;
    return THEFT_ALLOC_OK;
}

static void inode_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash inode_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct inode_case));
}

static void inode_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct inode_case *ic = instance;
    fprintf(f, "{mode=%d, maj=%u, min=%u}", ic->mode, ic->maj, ic->min);
}

static struct theft_type_info inode_case_info = {
    .alloc = inode_case_alloc_cb,
    .free = inode_case_free_cb,
    .hash = inode_case_hash_cb,
    .print = inode_case_print_cb,
};

static int dir_table_is_zeroed(const struct dirtb *dir)
{
    for (unsigned i = 0U; i < DIRTB_NUM; i++) {
        if (dir->tb[i] != NULL) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_constructs_expected_layout(struct theft *t, void *arg1)
{
    (void)t;
    const struct inode_case *ic = arg1;
    struct inode *node = malloc_inode(ic->mode, ic->maj, ic->min);
    int ok = node != NULL;

    if (ok) {
        ok = node->mode == (unsigned)ic->mode &&
             node->maj == ic->maj &&
             node->min == ic->min &&
             node->size == 0U &&
             node->mutex == 0 &&
             node->hd == NULL &&
             node->impl != NULL;

        if (ok) {
            if (ic->mode == DIR_MODE) {
                ok = node->dir != NULL && node->extents == NULL;
            } else {
                ok = node->dir == NULL && node->extents == NULL;
            }
        }
    }

    if (node != NULL) {
        dispose_inode(node);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_directory_table_is_zeroed(struct theft *t, void *arg1)
{
    (void)t;
    const struct inode_case *ic = arg1;
    struct inode *node = malloc_inode(ic->mode, ic->maj, ic->min);
    int ok = node != NULL;

    if (ok) {
        if (ic->mode == DIR_MODE) {
            ok = node->dir != NULL && node->extents == NULL && dir_table_is_zeroed(node->dir);
        } else {
            ok = node->dir == NULL && node->extents == NULL;
        }
    }

    if (node != NULL) {
        dispose_inode(node);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_allocations_do_not_alias(struct theft *t, void *arg1)
{
    (void)t;
    const struct inode_case *ic = arg1;
    struct inode *first = malloc_inode(ic->mode, ic->maj, ic->min);
    struct inode *second = malloc_inode(ic->mode, ic->maj, ic->min);
    int ok = first != NULL && second != NULL;

    if (ok) {
        ok = first != second &&
             first->impl != second->impl &&
             first->mode == second->mode &&
             first->maj == second->maj &&
             first->min == second->min &&
             first->size == second->size;

        if (ok) {
            if (ic->mode == DIR_MODE) {
                ok = first->dir != second->dir &&
                     first->extents == NULL && second->extents == NULL;
            } else {
                ok = first->dir == NULL && second->dir == NULL &&
                     first->extents == NULL && second->extents == NULL;
            }
        }
    }

    if (first != NULL) {
        dispose_inode(first);
    }
    if (second != NULL) {
        dispose_inode(second);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                       \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &inode_case_info },                                \
            .trials = trials_,                                                \
            .seed = theft_seed_of_time(),                                     \
        };                                                                    \
        enum theft_run_res res = theft_run(&cfg);                             \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) {                                          \
            failures++;                                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("malloc_inode extent property-based tests:\n");
    RUN_PROP("constructs_expected_layout", prop_constructs_expected_layout, 500);
    RUN_PROP("directory_table_is_zeroed", prop_directory_table_is_zeroed, 500);
    RUN_PROP("allocations_do_not_alias", prop_allocations_do_not_alias, 500);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
