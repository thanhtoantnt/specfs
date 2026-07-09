/*
 * Property-based tests for hash_func() in eval/pre_alloc/optimization/hashing.c
 *
 * Uses theft (PBT framework for C): https://github.com/silentbicycle/theft
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashing.h"

#define MAX_NAME_LEN 64

struct name_instance {
    size_t len;
    unsigned char buf[MAX_NAME_LEN + 1];
};

static unsigned char
non_nul_byte(struct theft *t)
{
    switch (theft_random_choice(t, 8)) {
    case 0:
        return (unsigned char)(0x80U + theft_random_choice(t, 0x80));
    case 1:
        return 0xffU;
    case 2:
        return 0x7fU;
    case 3:
        return (unsigned char)(1U + theft_random_choice(t, 31));
    default:
        return (unsigned char)(1U + theft_random_choice(t, 255));
    }
}

static enum theft_alloc_res
name_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct name_instance *ni = calloc(1, sizeof(*ni));
    if (ni == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8)) {
    case 0:
        ni->len = 0;
        break;
    case 1:
        ni->len = 1;
        break;
    case 2:
        ni->len = MAX_NAME_LEN;
        break;
    default:
        ni->len = (size_t)theft_random_choice(t, MAX_NAME_LEN + 1);
        break;
    }

    for (size_t i = 0; i < ni->len; i++) {
        ni->buf[i] = non_nul_byte(t);
    }
    ni->buf[ni->len] = '\0';

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
    return theft_hash_onepass((const uint8_t *)ni->buf, ni->len + 1);
}

static void
name_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct name_instance *ni = instance;

    fprintf(f, "len=%zu bytes=\"", ni->len);
    for (size_t i = 0; i < ni->len; i++) {
        unsigned char c = ni->buf[i];
        if (c >= 32 && c <= 126 && c != '\\' && c != '"') {
            fputc((int)c, f);
        } else {
            fprintf(f, "\\x%02x", c);
        }
    }
    fprintf(f, "\"");
}

static struct theft_type_info name_info = {
    .alloc = name_alloc_cb,
    .free = name_free_cb,
    .hash = name_hash_cb,
    .print = name_print_cb,
};

static unsigned int
reference_hash(const unsigned char *name)
{
    unsigned int hash = 0;
    while (*name != '\0') {
        hash = ((hash * 131U) + *name) & 0x1ffU;
        name++;
    }
    return hash;
}

static enum theft_trial_res
prop_matches_unsigned_byte_recurrence(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    unsigned int actual = hash_func((char *)ni->buf);
    unsigned int expected = reference_hash(ni->buf);

    if (actual != expected) {
        fprintf(stderr,
                "FAIL recurrence: expected %u, got %u for ",
                expected, actual);
        name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_output_is_9_bit(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    unsigned int actual = hash_func((char *)ni->buf);

    if (actual > 0x1ffU) {
        fprintf(stderr, "FAIL range: got %u for ", actual);
        name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_deterministic(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    unsigned int first = hash_func((char *)ni->buf);
    unsigned int second = hash_func((char *)ni->buf);

    if (first != second) {
        fprintf(stderr, "FAIL determinism: got %u then %u for ", first, second);
        name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_does_not_mutate_input(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    unsigned char before[MAX_NAME_LEN + 1];
    memcpy(before, ni->buf, sizeof(before));

    (void)hash_func((char *)ni->buf);

    if (memcmp(before, ni->buf, sizeof(before)) != 0) {
        fprintf(stderr, "FAIL mutation for ");
        name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_last_byte_extends_prefix(struct theft *t, void *arg1)
{
    (void)t;
    struct name_instance *ni = arg1;
    if (ni->len == 0) return THEFT_TRIAL_SKIP;

    unsigned char last = ni->buf[ni->len - 1];
    ni->buf[ni->len - 1] = '\0';
    unsigned int prefix_hash = hash_func((char *)ni->buf);
    ni->buf[ni->len - 1] = last;

    unsigned int actual = hash_func((char *)ni->buf);
    unsigned int expected = ((prefix_hash * 131U) + last) & 0x1ffU;

    if (actual != expected) {
        fprintf(stderr,
                "FAIL prefix extension: expected %u, got %u for ",
                expected, actual);
        name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &name_info },                                      \
            .trials = trials_,                                                \
            .seed = theft_seed_of_time(),                                     \
        };                                                                    \
        enum theft_run_res res = theft_run(&cfg);                             \
        printf("  [%s] %s\n",                                                \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);             \
        if (res != THEFT_RUN_PASS) failures++;                                \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("hash_func pre_alloc property-based tests:\n");
    RUN_PROP("matches_unsigned_byte_recurrence",
             prop_matches_unsigned_byte_recurrence, 500);
    RUN_PROP("output_is_9_bit", prop_output_is_9_bit, 500);
    RUN_PROP("deterministic", prop_deterministic, 250);
    RUN_PROP("does_not_mutate_input", prop_does_not_mutate_input, 250);
    RUN_PROP("last_byte_extends_prefix", prop_last_byte_extends_prefix, 500);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
