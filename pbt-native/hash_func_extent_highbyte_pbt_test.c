/*
 * Theft property-based tests for hash_func() in
 * eval/extent/optimization/hashing.c.
 *
 * These tests deliberately generate NUL-terminated byte strings containing
 * bytes above 127. This exercises the signed-char portability boundary in the
 * spec's byte-oriented multiplicative hash contract.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashing.h"

#define MAX_NAME_LEN 64

struct highbyte_name_instance {
    size_t len;
    size_t high_pos;
    unsigned char buf[MAX_NAME_LEN + 1];
};

static unsigned char
low_non_nul_byte(struct theft *t)
{
    switch (theft_random_choice(t, 8)) {
    case 0:
        return 1U;
    case 1:
        return 0x7fU;
    default:
        return (unsigned char)(1U + theft_random_choice(t, 0x7f));
    }
}

static unsigned char
high_non_nul_byte(struct theft *t)
{
    switch (theft_random_choice(t, 8)) {
    case 0:
        return 0x80U;
    case 1:
        return 0xffU;
    default:
        return (unsigned char)(0x80U + theft_random_choice(t, 0x80));
    }
}

static enum theft_alloc_res
highbyte_name_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct highbyte_name_instance *ni = calloc(1, sizeof(*ni));
    if (ni == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8)) {
    case 0:
        ni->len = 1;
        break;
    case 1:
        ni->len = 2;
        break;
    case 2:
        ni->len = MAX_NAME_LEN;
        break;
    default:
        ni->len = 1U + theft_random_choice(t, MAX_NAME_LEN);
        break;
    }

    ni->high_pos = theft_random_choice(t, ni->len);
    for (size_t i = 0; i < ni->len; i++) {
        ni->buf[i] = (i == ni->high_pos) ? high_non_nul_byte(t)
                                         : low_non_nul_byte(t);
    }
    ni->buf[ni->len] = '\0';

    *instance = ni;
    return THEFT_ALLOC_OK;
}

static void
highbyte_name_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash
highbyte_name_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct highbyte_name_instance *ni = instance;
    return theft_hash_onepass((const uint8_t *)ni->buf, ni->len + 1);
}

static void
highbyte_name_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct highbyte_name_instance *ni = instance;

    fprintf(f, "len=%zu high_pos=%zu bytes=\"", ni->len, ni->high_pos);
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

static struct theft_type_info highbyte_name_info = {
    .alloc = highbyte_name_alloc_cb,
    .free = highbyte_name_free_cb,
    .hash = highbyte_name_hash_cb,
    .print = highbyte_name_print_cb,
};

static int
contains_high_byte(const struct highbyte_name_instance *ni)
{
    for (size_t i = 0; i < ni->len; i++) {
        if (ni->buf[i] > 0x7fU) return 1;
    }
    return 0;
}

static unsigned int
reference_hash_unsigned_bytes(const unsigned char *name)
{
    unsigned int hash = 0;
    while (*name != '\0') {
        hash = ((hash * 131U) + *name) & 0x1ffU;
        name++;
    }
    return hash;
}

static enum theft_trial_res
prop_high_bytes_match_unsigned_byte_recurrence(struct theft *t, void *arg1)
{
    (void)t;
    struct highbyte_name_instance *ni = arg1;
    if (!contains_high_byte(ni)) {
        fprintf(stderr, "FAIL generator invariant: missing byte above 127 for ");
        highbyte_name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }

    unsigned int expected = reference_hash_unsigned_bytes(ni->buf);
    unsigned int actual = hash_func((char *)ni->buf);

    if (actual != expected) {
        fprintf(stderr, "FAIL unsigned-byte recurrence: expected %u, got %u for ",
                expected, actual);
        highbyte_name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_highbyte_output_is_9_bit(struct theft *t, void *arg1)
{
    (void)t;
    struct highbyte_name_instance *ni = arg1;
    unsigned int actual = hash_func((char *)ni->buf);

    if (actual > 0x1ffU) {
        fprintf(stderr, "FAIL range: got %u for ", actual);
        highbyte_name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_highbyte_deterministic(struct theft *t, void *arg1)
{
    (void)t;
    struct highbyte_name_instance *ni = arg1;
    unsigned int first = hash_func((char *)ni->buf);
    unsigned int second = hash_func((char *)ni->buf);

    if (first != second) {
        fprintf(stderr, "FAIL determinism: got %u then %u for ", first, second);
        highbyte_name_print_cb(stderr, ni, NULL);
        fprintf(stderr, "\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_highbyte_does_not_mutate_input(struct theft *t, void *arg1)
{
    (void)t;
    struct highbyte_name_instance *ni = arg1;
    unsigned char before[MAX_NAME_LEN + 1];
    memcpy(before, ni->buf, sizeof(before));

    (void)hash_func((char *)ni->buf);

    if (memcmp(before, ni->buf, sizeof(before)) != 0) {
        fprintf(stderr, "FAIL mutation for ");
        highbyte_name_print_cb(stderr, ni, NULL);
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
            .type_info = { &highbyte_name_info },                             \
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

    printf("hash_func extent high-byte property-based tests:\n");
    RUN_PROP("high_bytes_match_unsigned_byte_recurrence",
             prop_high_bytes_match_unsigned_byte_recurrence, 500);
    RUN_PROP("highbyte_output_is_9_bit", prop_highbyte_output_is_9_bit, 500);
    RUN_PROP("highbyte_deterministic", prop_highbyte_deterministic, 250);
    RUN_PROP("highbyte_does_not_mutate_input", prop_highbyte_does_not_mutate_input, 250);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
