/*
 * Property-based tests for calculate() in eval/loc/gen/util/calculate.c.
 *
 * Oracle: Differential/reference model for longest common path prefix plus
 * algebraic invariants for symmetry, maximality, NULL termination, and deep
 * copying. Stronger state-machine testing is not applicable: calculate() is a
 * pure one-shot transformation from two NULL-terminated path arrays to a newly
 * allocated NULL-terminated common-prefix array.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_PATH_COMPONENTS 8U
#define MAX_COMPONENT_LEN 8U

struct path_pair_instance {
    size_t src_len;
    size_t dst_len;
    char src_storage[MAX_PATH_COMPONENTS][MAX_COMPONENT_LEN + 1U];
    char dst_storage[MAX_PATH_COMPONENTS][MAX_COMPONENT_LEN + 1U];
    char *src[MAX_PATH_COMPONENTS + 1U];
    char *dst[MAX_PATH_COMPONENTS + 1U];
};

static void fill_component(struct theft *t, char out[MAX_COMPONENT_LEN + 1U])
{
    uint64_t len = theft_random_choice(t, MAX_COMPONENT_LEN + 1U);
    for (uint64_t i = 0; i < len; i++) {
        out[i] = (char)('a' + theft_random_choice(t, 4U));
    }
    out[len] = '\0';
}

static void init_path_views(struct path_pair_instance *pair)
{
    for (size_t i = 0; i < pair->src_len; i++) {
        pair->src[i] = pair->src_storage[i];
    }
    pair->src[pair->src_len] = NULL;

    for (size_t i = 0; i < pair->dst_len; i++) {
        pair->dst[i] = pair->dst_storage[i];
    }
    pair->dst[pair->dst_len] = NULL;
}

static enum theft_alloc_res path_pair_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct path_pair_instance *pair = calloc(1, sizeof(*pair));
    if (pair == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    pair->src_len = theft_random_choice(t, MAX_PATH_COMPONENTS + 1U);
    pair->dst_len = theft_random_choice(t, MAX_PATH_COMPONENTS + 1U);
    size_t min_len = pair->src_len < pair->dst_len ? pair->src_len : pair->dst_len;
    size_t shared_len = min_len == 0U ? 0U : theft_random_choice(t, min_len + 1U);

    for (size_t i = 0; i < shared_len; i++) {
        fill_component(t, pair->src_storage[i]);
        strcpy(pair->dst_storage[i], pair->src_storage[i]);
    }
    for (size_t i = shared_len; i < pair->src_len; i++) {
        fill_component(t, pair->src_storage[i]);
    }
    for (size_t i = shared_len; i < pair->dst_len; i++) {
        fill_component(t, pair->dst_storage[i]);
    }

    init_path_views(pair);
    *instance = pair;
    return THEFT_ALLOC_OK;
}

static void path_pair_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash path_pair_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct path_pair_instance *pair = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&pair->src_len, sizeof(pair->src_len));
    theft_hash_sink(&h, (const uint8_t *)&pair->dst_len, sizeof(pair->dst_len));
    for (size_t i = 0; i < pair->src_len; i++) {
        theft_hash_sink(&h, (const uint8_t *)pair->src_storage[i], strlen(pair->src_storage[i]) + 1U);
    }
    for (size_t i = 0; i < pair->dst_len; i++) {
        theft_hash_sink(&h, (const uint8_t *)pair->dst_storage[i], strlen(pair->dst_storage[i]) + 1U);
    }
    return theft_hash_done(&h);
}

static void path_pair_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct path_pair_instance *pair = instance;
    fprintf(f, "src=[");
    for (size_t i = 0; i < pair->src_len; i++) {
        fprintf(f, "%s\"%s\"", i == 0U ? "" : ", ", pair->src_storage[i]);
    }
    fprintf(f, "], dst=[");
    for (size_t i = 0; i < pair->dst_len; i++) {
        fprintf(f, "%s\"%s\"", i == 0U ? "" : ", ", pair->dst_storage[i]);
    }
    fprintf(f, "]");
}

static struct theft_type_info path_pair_info = {
    .alloc = path_pair_alloc_cb,
    .free = path_pair_free_cb,
    .hash = path_pair_hash_cb,
    .print = path_pair_print_cb,
};

static size_t reference_common_prefix_len(const struct path_pair_instance *pair)
{
    size_t len = 0;
    while (len < pair->src_len && len < pair->dst_len &&
           strcmp(pair->src_storage[len], pair->dst_storage[len]) == 0) {
        len++;
    }
    return len;
}

static size_t result_len(char **path)
{
    size_t len = 0;
    while (path[len] != NULL) {
        len++;
    }
    return len;
}

static int result_matches_prefix(char **result, char *expected[], size_t expected_len)
{
    if (result == NULL || result_len(result) != expected_len) {
        return 0;
    }
    for (size_t i = 0; i < expected_len; i++) {
        if (strcmp(result[i], expected[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_matches_reference_prefix(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    char **result = calculate(pair->src, pair->dst);
    size_t expected_len = reference_common_prefix_len(pair);
    int ok = result_matches_prefix(result, pair->src, expected_len);
    free_path(result);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_prefix_is_maximal(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    char **result = calculate(pair->src, pair->dst);
    if (result == NULL) {
        return THEFT_TRIAL_FAIL;
    }

    size_t len = result_len(result);
    int ok = !(len < pair->src_len && len < pair->dst_len &&
               strcmp(pair->src_storage[len], pair->dst_storage[len]) == 0);
    free_path(result);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_result_is_null_terminated(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    char **result = calculate(pair->src, pair->dst);
    size_t expected_len = reference_common_prefix_len(pair);
    int ok = result != NULL && result[expected_len] == NULL;
    free_path(result);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_result_is_deep_copy(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    char **result = calculate(pair->src, pair->dst);
    size_t expected_len = reference_common_prefix_len(pair);
    if (result == NULL) {
        return THEFT_TRIAL_FAIL;
    }

    int ok = 1;
    for (size_t i = 0; i < expected_len; i++) {
        if (result[i] == pair->src[i] || result[i] == pair->dst[i]) {
            ok = 0;
            break;
        }
    }

    if (ok && expected_len > 0U) {
        char before[MAX_COMPONENT_LEN + 1U];
        strcpy(before, result[0]);
        pair->src_storage[0][0] = pair->src_storage[0][0] == 'z' ? 'y' : 'z';
        ok = strcmp(result[0], before) == 0;
    }

    free_path(result);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_symmetric_by_value(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    char **forward = calculate(pair->src, pair->dst);
    char **reverse = calculate(pair->dst, pair->src);

    int ok = forward != NULL && reverse != NULL && result_len(forward) == result_len(reverse);
    if (ok) {
        size_t len = result_len(forward);
        for (size_t i = 0; i < len; i++) {
            if (strcmp(forward[i], reverse[i]) != 0) {
                ok = 0;
                break;
            }
        }
    }

    free_path(forward);
    free_path(reverse);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &path_pair_info },                                 \
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
    printf("loc calculate property-based tests:\n");
    RUN_PROP("matches_reference_prefix", prop_matches_reference_prefix, 500);
    RUN_PROP("prefix_is_maximal", prop_prefix_is_maximal, 500);
    RUN_PROP("result_is_null_terminated", prop_result_is_null_terminated, 500);
    RUN_PROP("result_is_deep_copy", prop_result_is_deep_copy, 500);
    RUN_PROP("symmetric_by_value", prop_symmetric_by_value, 500);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
