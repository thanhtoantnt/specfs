/*
 * Property-based tests for malloc_getattr_ret() in
 * eval/extent/baseline/util.c
 *
 * Oracle: Algebraic — Invariant (4d)
 * Stronger considered:
 *   - State Machine (3): rejected — malloc_getattr_ret is a single allocator with no lifecycle state
 *   - Differential (7): rejected — no independent trusted implementation in the baseline target
 *   - Round-trip (4a): rejected — no inverse/free-with-observable-state pair for getattr_ret
 * Weaker available: Negative/Error Contract (4e), Crash-Only (6)
 *
 * Uses theft (PBT framework for C): https://github.com/silentbicycle/theft
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "util.h"

struct getattr_case {
    struct inode node;
    unsigned mode;
    unsigned size;
    unsigned maj;
    unsigned min;
};

static unsigned
choice_u32(struct theft *t)
{
    unsigned value = 0U;
    for (unsigned i = 0; i < sizeof(value); i++) {
        value = (value << 8U) | (unsigned)theft_random_choice(t, 256U);
    }
    return value;
}

static unsigned
edge_or_random_u32(struct theft *t)
{
    switch (theft_random_choice(t, 12U)) {
    case 0: return 0U;
    case 1: return 1U;
    case 2: return 2U;
    case 3: return PG_SIZE - 1U;
    case 4: return PG_SIZE;
    case 5: return PG_SIZE + 1U;
    case 6: return MAX_FILE_SIZE - 1U;
    case 7: return MAX_FILE_SIZE;
    case 8: return UINT32_MAX - 1U;
    case 9: return UINT32_MAX;
    default: return choice_u32(t);
    }
}

static unsigned
valid_mode(struct theft *t)
{
    switch (theft_random_choice(t, 6U)) {
    case 0: return FILE_MODE;
    case 1: return DIR_MODE;
    case 2: return CHR_MODE;
    case 3: return BLK_MODE;
    case 4: return SOCK_MODE;
    default: return FIFO_MODE;
    }
}

static enum theft_alloc_res
getattr_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct getattr_case *gc = malloc(sizeof(*gc));
    if (gc == NULL) return THEFT_ALLOC_ERROR;

    memset(gc, 0, sizeof(*gc));
    gc->mode = valid_mode(t);
    gc->size = edge_or_random_u32(t);
    gc->maj = edge_or_random_u32(t);
    gc->min = edge_or_random_u32(t);

    gc->node.mutex = (int)edge_or_random_u32(t);
    gc->node.impl = (struct mcs_mutex *)(uintptr_t)edge_or_random_u32(t);
    gc->node.hd = (struct mcs_node *)(uintptr_t)edge_or_random_u32(t);
    gc->node.maj = edge_or_random_u32(t);
    gc->node.min = edge_or_random_u32(t);
    gc->node.mode = valid_mode(t);
    gc->node.size = edge_or_random_u32(t);
    gc->node.dir = (struct dirtb *)(uintptr_t)edge_or_random_u32(t);
    gc->node.file = (struct indextb *)(uintptr_t)edge_or_random_u32(t);

    *instance = gc;
    return THEFT_ALLOC_OK;
}

static void
getattr_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash
getattr_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct getattr_case));
}

static void
getattr_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct getattr_case *gc = instance;
    fprintf(f, "{mode=%u, size=%u, maj=%u, min=%u}",
            gc->mode, gc->size, gc->maj, gc->min);
}

static struct theft_type_info getattr_case_info = {
    .alloc = getattr_case_alloc_cb,
    .free = getattr_case_free_cb,
    .hash = getattr_case_hash_cb,
    .print = getattr_case_print_cb,
};

static enum theft_trial_res
prop_common_fields_match_inputs(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;

    struct getattr_ret *ret = malloc_getattr_ret((struct inode *)&gc->node,
                                                 gc->mode, gc->size,
                                                 gc->maj, gc->min);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->inum == &gc->node &&
             ret->mode == gc->mode &&
             ret->size == gc->size;
    if (!ok) {
        fprintf(stderr,
                "common fields mismatch: got inum=%p mode=%u size=%u; expected inum=%p mode=%u size=%u\n",
                (void *)ret->inum, ret->mode, ret->size,
                (void *)&gc->node, gc->mode, gc->size);
    }

    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_directory_device_numbers_are_zero(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;

    struct getattr_ret *ret = malloc_getattr_ret((struct inode *)&gc->node,
                                                 DIR_MODE, gc->size,
                                                 gc->maj, gc->min);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->maj == 0U && ret->min == 0U;
    if (!ok) {
        fprintf(stderr,
                "directory device numbers not zeroed: got maj=%u min=%u from input maj=%u min=%u\n",
                ret->maj, ret->min, gc->maj, gc->min);
    }

    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_non_directory_device_numbers_are_preserved(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;
    if (gc->mode == DIR_MODE) return THEFT_TRIAL_SKIP;

    struct getattr_ret *ret = malloc_getattr_ret((struct inode *)&gc->node,
                                                 gc->mode, gc->size,
                                                 gc->maj, gc->min);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->maj == gc->maj && ret->min == gc->min;
    if (!ok) {
        fprintf(stderr,
                "non-directory device numbers changed: got maj=%u min=%u expected maj=%u min=%u mode=%u\n",
                ret->maj, ret->min, gc->maj, gc->min, gc->mode);
    }

    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_input_inode_is_not_mutated(struct theft *t, void *arg1)
{
    (void)t;
    struct getattr_case *gc = arg1;
    struct inode before = gc->node;

    struct getattr_ret *ret = malloc_getattr_ret(&gc->node, gc->mode, gc->size,
                                                 gc->maj, gc->min);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = memcmp(&gc->node, &before, sizeof(before)) == 0;
    if (!ok) {
        fprintf(stderr, "malloc_getattr_ret mutated input inode\n");
    }

    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_each_call_returns_independent_allocation(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;

    struct getattr_ret *first = malloc_getattr_ret((struct inode *)&gc->node,
                                                   gc->mode, gc->size,
                                                   gc->maj, gc->min);
    struct getattr_ret *second = malloc_getattr_ret((struct inode *)&gc->node,
                                                    gc->mode, gc->size,
                                                    gc->maj, gc->min);
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return THEFT_TRIAL_ERROR;
    }

    int ok = first != second &&
             first->inum == second->inum &&
             first->mode == second->mode &&
             first->size == second->size &&
             first->maj == second->maj &&
             first->min == second->min;
    if (!ok) {
        fprintf(stderr, "allocations not independent or contents differ: first=%p second=%p\n",
                (void *)first, (void *)second);
    }

    free(first);
    free(second);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                 \
        struct theft_run_config cfg = {                                  \
            .name = name_,                                               \
            .prop1 = prop_,                                              \
            .type_info = { &getattr_case_info },                         \
            .trials = trials_,                                           \
            .seed = theft_seed_of_time(),                                \
        };                                                               \
        enum theft_run_res res = theft_run(&cfg);                        \
        printf("  [%s] %s\n",                                           \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);         \
        if (res != THEFT_RUN_PASS) failures++;                           \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("malloc_getattr_ret baseline property-based tests:\n");
    RUN_PROP("common_fields_match_inputs", prop_common_fields_match_inputs, 300);
    RUN_PROP("directory_device_numbers_are_zero", prop_directory_device_numbers_are_zero, 300);
    RUN_PROP("non_directory_device_numbers_are_preserved", prop_non_directory_device_numbers_are_preserved, 300);
    RUN_PROP("input_inode_is_not_mutated", prop_input_inode_is_not_mutated, 200);
    RUN_PROP("each_call_returns_independent_allocation", prop_each_call_returns_independent_allocation, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
