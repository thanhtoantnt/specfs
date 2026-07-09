/*
 * Property-based tests for hash_func() in eval/delay_alloc/optimization/hashing.c
 *
 * Uses theft (PBT framework for C): https://github.com/silentbicycle/theft
 *
 * Contract from the Coq spec:
 *   hash_func(name) returns a 9-bit value in [0, 511].
 *   Deterministic: same input always produces same output.
 *   No side effects: does not modify `name`.
 *
 * The Coq range lemma is "Admitted" — not proven. These properties pin the
 * runtime contract so any regression or overflow is caught.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashing.h"

/* ---- String generator (for theft) ---- */

#define MAX_NAME_LEN 32

struct name_instance {
    char buf[MAX_NAME_LEN + 1];
};

static enum theft_alloc_res
name_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct name_instance *ni = malloc(sizeof(*ni));
    if (ni == NULL) return THEFT_ALLOC_ERROR;

    /* Length 0..MAX_NAME_LEN */
    uint64_t len = theft_random_choice(t, MAX_NAME_LEN + 1);
    for (uint64_t i = 0; i < len; i++) {
        /* Printable ASCII 33..126 (avoid NUL) */
        ni->buf[i] = (char)(33 + theft_random_choice(t, 94));
    }
    ni->buf[len] = '\0';
    *instance = ni;
    return THEFT_ALLOC_OK;
}

static void
name_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash
name_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct name_instance *ni = instance;
    return theft_hash_onepass((const uint8_t *)ni->buf, strlen(ni->buf));
}

static void
name_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct name_instance *ni = instance;
    fprintf(f, "\"%s\"", ni->buf);
}

static struct theft_type_info name_info = {
    .alloc  = name_alloc_cb,
    .free   = name_free_cb,
    .hash   = name_hash_cb,
    .print  = name_print_cb,
};

/* ---- Property 1: Output is always in [0, 511] (9-bit range) ---- */

static enum theft_trial_res
prop_output_in_range(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    unsigned int h = hash_func(ni->buf);
    if (h > 511) {
        fprintf(stderr, "FAIL range: hash_func(\"%s\") = %u (expected <= 511)\n",
                ni->buf, h);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ---- Property 2: Determinism (same input → same output) ---- */

static enum theft_trial_res
prop_deterministic(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    unsigned int h1 = hash_func(ni->buf);
    unsigned int h2 = hash_func(ni->buf);
    if (h1 != h2) {
        fprintf(stderr, "FAIL determinism: hash_func(\"%s\") = %u then %u\n",
                ni->buf, h1, h2);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ---- Property 3: Does not modify the input string ---- */

static enum theft_trial_res
prop_no_mutation(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    char before[MAX_NAME_LEN + 1];
    strcpy(before, ni->buf);
    hash_func(ni->buf);
    if (strcmp(before, ni->buf) != 0) {
        fprintf(stderr, "FAIL mutation: input \"%s\" changed to \"%s\"\n",
                before, ni->buf);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ---- Property 4: Empty string → 0 ---- */

static enum theft_trial_res
prop_empty_string_is_zero(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    if (strlen(ni->buf) != 0) return THEFT_TRIAL_SKIP;  /* only test empty */
    if (hash_func(ni->buf) != 0) {
        fprintf(stderr, "FAIL empty: hash_func(\"\") = %u (expected 0)\n",
                hash_func(ni->buf));
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
            .type_info = { &name_info },                                \
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

    printf("hash_func property-based tests:\n");
    RUN_PROP("output_in_range",    prop_output_in_range,    500);
    RUN_PROP("deterministic",      prop_deterministic,      200);
    RUN_PROP("no_mutation",        prop_no_mutation,        200);
    RUN_PROP("empty_string_is_0",  prop_empty_string_is_zero, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
