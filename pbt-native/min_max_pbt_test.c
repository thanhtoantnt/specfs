/*
 * Property-based tests for min() and max() in
 * eval/delay_alloc/optimization/util.c
 *
 * Oracle: Algebraic - ordered-pair invariant for a total order.
 * Stronger considered:
 *   - State Machine: rejected, min/max are pure stateless functions.
 *   - Differential: rejected, there is no independent oracle stronger than the
 *     algebraic laws for these one-line helpers.
 * Weaker available: Reference, Crash-Only.
 */
#include <theft.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "util.h"

void brels(struct indextb *tb)
{
    (void)tb;
}

struct minmax_case {
    unsigned a;
    unsigned b;
    unsigned c;
};

static unsigned draw_edge_unsigned(struct theft *t)
{
    switch (theft_random_choice(t, 10U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return 2U;
    case 3:
        return UINT_MAX;
    case 4:
        return UINT_MAX - 1U;
    case 5:
        return UINT_MAX / 2U;
    case 6:
        return UINT_MAX / 2U + 1U;
    default:
        return (unsigned)theft_random_choice(t, (uint64_t)UINT_MAX + 1ULL);
    }
}

static enum theft_alloc_res minmax_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct minmax_case *mc = calloc(1U, sizeof(*mc));
    if (mc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    mc->a = draw_edge_unsigned(t);
    mc->b = draw_edge_unsigned(t);
    mc->c = draw_edge_unsigned(t);

    *instance = mc;
    return THEFT_ALLOC_OK;
}

static void minmax_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash minmax_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct minmax_case *mc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&mc->a, sizeof(mc->a));
    theft_hash_sink(&h, (const uint8_t *)&mc->b, sizeof(mc->b));
    theft_hash_sink(&h, (const uint8_t *)&mc->c, sizeof(mc->c));
    return theft_hash_done(&h);
}

static void minmax_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct minmax_case *mc = instance;
    fprintf(f, "{a=%u, b=%u, c=%u}", mc->a, mc->b, mc->c);
}

static struct theft_type_info minmax_case_info = {
    .alloc = minmax_case_alloc_cb,
    .free = minmax_case_free_cb,
    .hash = minmax_case_hash_cb,
    .print = minmax_case_print_cb,
};

static enum theft_trial_res prop_commutative(struct theft *t, void *arg1)
{
    (void)t;
    const struct minmax_case *mc = arg1;

    unsigned min_ab = min(mc->a, mc->b);
    unsigned min_ba = min(mc->b, mc->a);
    unsigned max_ab = max(mc->a, mc->b);
    unsigned max_ba = max(mc->b, mc->a);

    return (min_ab == min_ba && max_ab == max_ba) ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_pair_is_sorted_and_lossless(struct theft *t, void *arg1)
{
    (void)t;
    const struct minmax_case *mc = arg1;

    unsigned lo = min(mc->a, mc->b);
    unsigned hi = max(mc->a, mc->b);
    uint64_t observed_sum = (uint64_t)lo + (uint64_t)hi;
    uint64_t expected_sum = (uint64_t)mc->a + (uint64_t)mc->b;
    int ok = lo <= hi &&
             observed_sum == expected_sum &&
             (lo == mc->a || lo == mc->b) &&
             (hi == mc->a || hi == mc->b);

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_idempotent(struct theft *t, void *arg1)
{
    (void)t;
    const struct minmax_case *mc = arg1;

    int ok = min(mc->a, mc->a) == mc->a &&
             max(mc->a, mc->a) == mc->a &&
             min(mc->b, mc->b) == mc->b &&
             max(mc->b, mc->b) == mc->b &&
             min(mc->c, mc->c) == mc->c &&
             max(mc->c, mc->c) == mc->c;

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_associative(struct theft *t, void *arg1)
{
    (void)t;
    const struct minmax_case *mc = arg1;

    unsigned min_left = min(min(mc->a, mc->b), mc->c);
    unsigned min_right = min(mc->a, min(mc->b, mc->c));
    unsigned max_left = max(max(mc->a, mc->b), mc->c);
    unsigned max_right = max(mc->a, max(mc->b, mc->c));

    return (min_left == min_right && max_left == max_right) ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                       \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &minmax_case_info },                               \
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
    printf("min/max delay_alloc property-based tests:\n");
    RUN_PROP("commutative", prop_commutative, 800);
    RUN_PROP("pair_is_sorted_and_lossless", prop_pair_is_sorted_and_lossless, 800);
    RUN_PROP("idempotent", prop_idempotent, 800);
    RUN_PROP("associative", prop_associative, 800);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
