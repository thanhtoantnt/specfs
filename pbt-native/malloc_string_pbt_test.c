/*
 * Property-based tests for malloc_string() in
 * eval/delay_alloc/optimization/util.c
 *
 * Oracle: Algebraic - copy/deep-copy invariant
 * Stronger considered:
 *   - State Machine: rejected, malloc_string is stateless and single-shot.
 *   - Differential: rejected, strdup is similar but not an independent project oracle.
 * Weaker available: Reference, Crash-Only
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "util.h"

#define MAX_TEST_STRING_LEN 512U

void brels(struct indextb *tb)
{
    (void)tb;
}

struct string_case {
    size_t len;
    char bytes[MAX_TEST_STRING_LEN + 1U];
};

static char draw_non_nul_byte(struct theft *t)
{
    switch (theft_random_choice(t, 12U)) {
    case 0:
        return 'a';
    case 1:
        return 'Z';
    case 2:
        return '0';
    case 3:
        return '/';
    case 4:
        return ' ';
    case 5:
        return '\t';
    case 6:
        return (char)0x7f;
    case 7:
        return (char)0x80;
    case 8:
        return (char)0xff;
    default:
        return (char)(1U + theft_random_choice(t, 255U));
    }
}

static size_t draw_len(struct theft *t)
{
    switch (theft_random_choice(t, 10U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return 2U;
    case 3:
        return MAX_TEST_STRING_LEN - 1U;
    case 4:
        return MAX_TEST_STRING_LEN;
    default:
        return (size_t)theft_random_choice(t, MAX_TEST_STRING_LEN + 1U);
    }
}

static enum theft_alloc_res string_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct string_case *sc = calloc(1U, sizeof(*sc));
    if (sc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    sc->len = draw_len(t);
    for (size_t i = 0U; i < sc->len; i++) {
        sc->bytes[i] = draw_non_nul_byte(t);
        if (sc->bytes[i] == '\0') {
            sc->bytes[i] = 'x';
        }
    }
    sc->bytes[sc->len] = '\0';

    *instance = sc;
    return THEFT_ALLOC_OK;
}

static void string_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash string_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct string_case *sc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&sc->len, sizeof(sc->len));
    theft_hash_sink(&h, sc->bytes, sc->len + 1U);
    return theft_hash_done(&h);
}

static void string_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct string_case *sc = instance;
    fprintf(f, "{len=%zu, bytes=\"", sc->len);
    for (size_t i = 0U; i < sc->len; i++) {
        unsigned char c = (unsigned char)sc->bytes[i];
        if (c >= 32U && c <= 126U && c != '"' && c != '\\') {
            fputc((int)c, f);
        } else {
            fprintf(f, "\\x%02x", c);
        }
    }
    fprintf(f, "\"}");
}

static struct theft_type_info string_case_info = {
    .alloc = string_case_alloc_cb,
    .free = string_case_free_cb,
    .hash = string_case_hash_cb,
    .print = string_case_print_cb,
};

static enum theft_trial_res prop_exact_copy_including_terminator(struct theft *t, void *arg1)
{
    (void)t;
    const struct string_case *sc = arg1;
    char *copy = malloc_string(sc->bytes);
    int ok = copy != NULL &&
             copy != sc->bytes &&
             strlen(copy) == sc->len &&
             memcmp(copy, sc->bytes, sc->len + 1U) == 0;

    free(copy);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_copy_is_independent_of_source(struct theft *t, void *arg1)
{
    (void)t;
    const struct string_case *sc = arg1;
    char source[MAX_TEST_STRING_LEN + 1U];
    memcpy(source, sc->bytes, sc->len + 1U);

    char *copy = malloc_string(source);
    int ok = copy != NULL && memcmp(copy, source, sc->len + 1U) == 0;

    if (ok && sc->len > 0U) {
        char original = copy[0];
        source[0] = source[0] == 'Q' ? 'R' : 'Q';
        ok = copy[0] == original;
    }

    free(copy);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_result_buffer_is_mutable_without_source_alias(struct theft *t, void *arg1)
{
    (void)t;
    const struct string_case *sc = arg1;
    char source[MAX_TEST_STRING_LEN + 1U];
    memcpy(source, sc->bytes, sc->len + 1U);

    char *copy = malloc_string(source);
    int ok = copy != NULL && copy != source;

    if (ok && sc->len > 0U) {
        copy[0] = copy[0] == 'M' ? 'N' : 'M';
        ok = source[0] == sc->bytes[0];
    }
    if (ok) {
        ok = source[sc->len] == '\0' && copy[sc->len] == '\0';
    }

    free(copy);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_repeated_allocations_do_not_alias(struct theft *t, void *arg1)
{
    (void)t;
    const struct string_case *sc = arg1;
    char *first = malloc_string(sc->bytes);
    char *second = malloc_string(sc->bytes);
    int ok = first != NULL && second != NULL && first != second;

    if (ok) {
        ok = memcmp(first, sc->bytes, sc->len + 1U) == 0 &&
             memcmp(second, sc->bytes, sc->len + 1U) == 0;
    }
    if (ok && sc->len > 0U) {
        first[0] = first[0] == 'A' ? 'B' : 'A';
        ok = second[0] == sc->bytes[0];
    }

    free(first);
    free(second);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                       \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &string_case_info },                               \
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
    printf("malloc_string delay_alloc property-based tests:\n");
    RUN_PROP("exact_copy_including_terminator", prop_exact_copy_including_terminator, 1000);
    RUN_PROP("copy_is_independent_of_source", prop_copy_is_independent_of_source, 1000);
    RUN_PROP("result_buffer_is_mutable_without_source_alias", prop_result_buffer_is_mutable_without_source_alias, 1000);
    RUN_PROP("repeated_allocations_do_not_alias", prop_repeated_allocations_do_not_alias, 1000);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
