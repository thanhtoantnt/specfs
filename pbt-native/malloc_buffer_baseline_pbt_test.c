/*
 * Theft property-based tests for malloc_buffer() in
 * eval/extent/baseline/util.c
 *
 * Oracle: Algebraic — allocator invariant / storage contract.
 * Spec evidence: sysspec/specfs/util/malloc_buffer.spec states the
 * post-condition: "Allocate a buffer of length `len` and return a pointer to
 * the buffer."  The implementation is not documented to zero-initialize the
 * bytes, so these properties intentionally assert allocation size, writable
 * storage, independence, and boundary support only.
 * Stronger considered:
 *   - State Machine: rejected — malloc_buffer is a single allocation helper
 *     with no lifecycle state beyond caller-owned free().
 *   - Differential: rejected — malloc(3) is the primitive contract being
 *     wrapped, not an independent project implementation.
 *   - Round-trip: rejected — there is no inverse with observable semantics
 *     beyond free().
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "util.h"

#define MAX_TEST_BUFFER_LEN 8192U
#define CANARY_SIZE 32U
#define CANARY_BYTE 0xa5U

static void *(*real_malloc_fn)(size_t) = malloc;
static void (*real_free_fn)(void *) = free;
static void *(*real_memset_fn)(void *, int, size_t) = memset;

struct buffer_case {
    unsigned len;
    unsigned pattern_seed;
};

struct tracked_alloc {
    unsigned char *raw;
    void *user;
    size_t requested;
};

static struct tracked_alloc last_alloc;
static unsigned malloc_calls;
static int tracker_enabled;

static unsigned draw_u32(struct theft *t)
{
    unsigned value = 0U;
    for (unsigned i = 0U; i < sizeof(value); i++) {
        value = (value << 8U) | (unsigned)theft_random_choice(t, 256U);
    }
    return value;
}

static unsigned draw_len(struct theft *t)
{
    switch (theft_random_choice(t, 12U)) {
    case 0: return 0U;
    case 1: return 1U;
    case 2: return 2U;
    case 3: return 3U;
    case 4: return PG_SIZE - 1U;
    case 5: return PG_SIZE;
    case 6: return PG_SIZE + 1U;
    case 7: return MAX_TEST_BUFFER_LEN - 1U;
    case 8: return MAX_TEST_BUFFER_LEN;
    default: return (unsigned)theft_random_choice(t, MAX_TEST_BUFFER_LEN + 1U);
    }
}

static enum theft_alloc_res buffer_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct buffer_case *bc = calloc(1U, sizeof(*bc));
    if (bc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    bc->len = draw_len(t);
    bc->pattern_seed = draw_u32(t);

    *instance = bc;
    return THEFT_ALLOC_OK;
}

static void buffer_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash buffer_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct buffer_case));
}

static void buffer_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct buffer_case *bc = instance;
    fprintf(f, "{len=%u, pattern_seed=%u}", bc->len, bc->pattern_seed);
}

static struct theft_type_info buffer_case_info = {
    .alloc = buffer_case_alloc_cb,
    .free = buffer_case_free_cb,
    .hash = buffer_case_hash_cb,
    .print = buffer_case_print_cb,
};

static void reset_tracker(void)
{
    memset(&last_alloc, 0, sizeof(last_alloc));
    malloc_calls = 0U;
}

static void *tracked_malloc(size_t size)
{
    if (!tracker_enabled) {
        return real_malloc_fn(size);
    }

    malloc_calls++;
    size_t total = CANARY_SIZE + size + CANARY_SIZE;
    unsigned char *raw = real_malloc_fn(total == 0U ? 1U : total);
    if (raw == NULL) {
        return NULL;
    }

    real_memset_fn(raw, CANARY_BYTE, total == 0U ? 1U : total);
    last_alloc.raw = raw;
    last_alloc.user = raw + CANARY_SIZE;
    last_alloc.requested = size;
    return last_alloc.user;
}

static void tracked_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    if (ptr == last_alloc.user && last_alloc.raw != NULL) {
        real_free_fn(last_alloc.raw);
        memset(&last_alloc, 0, sizeof(last_alloc));
        return;
    }

    real_free_fn(ptr);
}

#define malloc tracked_malloc
#include "../eval/extent/baseline/util.c"
#undef malloc

static int canaries_intact(void)
{
    if (last_alloc.raw == NULL) {
        return 0;
    }

    for (size_t i = 0U; i < CANARY_SIZE; i++) {
        if (last_alloc.raw[i] != CANARY_BYTE) {
            return 0;
        }
    }
    for (size_t i = 0U; i < CANARY_SIZE; i++) {
        if (last_alloc.raw[CANARY_SIZE + last_alloc.requested + i] != CANARY_BYTE) {
            return 0;
        }
    }
    return 1;
}

static unsigned char byte_for(const struct buffer_case *bc, unsigned index, unsigned salt)
{
    unsigned mixed = bc->pattern_seed ^ (index * 1103515245U) ^ salt;
    return (unsigned char)(mixed & 0xffU);
}

static enum theft_trial_res prop_allocates_exact_byte_count(struct theft *t, void *arg1)
{
    (void)t;
    const struct buffer_case *bc = arg1;

    reset_tracker();
    tracker_enabled = 1;
    char *buf = malloc_buffer(bc->len);
    tracker_enabled = 0;

    int ok = buf != NULL &&
             buf == (char *)last_alloc.user &&
             malloc_calls == 1U &&
             last_alloc.requested == (size_t)bc->len &&
             canaries_intact();
    if (!ok) {
        fprintf(stderr,
                "allocation mismatch: len=%u buf=%p user=%p calls=%u requested=%zu\n",
                bc->len, (void *)buf, last_alloc.user, malloc_calls,
                last_alloc.requested);
    }

    tracked_free(buf);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_returned_range_is_writable(struct theft *t, void *arg1)
{
    (void)t;
    const struct buffer_case *bc = arg1;

    reset_tracker();
    tracker_enabled = 1;
    char *buf = malloc_buffer(bc->len);
    tracker_enabled = 0;
    if (buf == NULL) return THEFT_TRIAL_ERROR;

    for (unsigned i = 0U; i < bc->len; i++) {
        buf[i] = (char)byte_for(bc, i, 0x13579bdfU);
    }

    int ok = canaries_intact();
    for (unsigned i = 0U; ok && i < bc->len; i++) {
        ok = (unsigned char)buf[i] == byte_for(bc, i, 0x13579bdfU);
    }
    if (!ok) {
        fprintf(stderr, "writable range/canary failure for len=%u\n", bc->len);
    }

    tracked_free(buf);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_each_call_returns_independent_storage(struct theft *t, void *arg1)
{
    (void)t;
    const struct buffer_case *bc = arg1;
    if (bc->len == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    char *first = malloc_buffer(bc->len);
    char *second = malloc_buffer(bc->len);
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return THEFT_TRIAL_ERROR;
    }

    for (unsigned i = 0U; i < bc->len; i++) {
        first[i] = (char)byte_for(bc, i, 0x2468ace0U);
        second[i] = (char)byte_for(bc, i, 0xfdb97531U);
    }

    int ok = first != second;
    for (unsigned i = 0U; ok && i < bc->len; i++) {
        ok = (unsigned char)first[i] == byte_for(bc, i, 0x2468ace0U) &&
             (unsigned char)second[i] == byte_for(bc, i, 0xfdb97531U);
    }
    if (!ok) {
        fprintf(stderr, "allocations alias or corrupt each other for len=%u\n", bc->len);
    }

    free(first);
    free(second);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_boundary_lengths_are_supported(struct theft *t, void *arg1)
{
    (void)t;
    const struct buffer_case *bc = arg1;
    if (!(bc->len == 0U || bc->len == 1U || bc->len == PG_SIZE - 1U ||
          bc->len == PG_SIZE || bc->len == PG_SIZE + 1U ||
          bc->len == MAX_TEST_BUFFER_LEN)) {
        return THEFT_TRIAL_SKIP;
    }

    char *buf = malloc_buffer(bc->len);
    if (buf == NULL) return THEFT_TRIAL_ERROR;

    for (unsigned i = 0U; i < bc->len; i++) {
        buf[i] = (char)byte_for(bc, i, 0xabcdef01U);
    }

    int ok = 1;
    for (unsigned i = 0U; ok && i < bc->len; i++) {
        ok = (unsigned char)buf[i] == byte_for(bc, i, 0xabcdef01U);
    }

    free(buf);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                 \
        struct theft_run_config cfg = {                                  \
            .name = name_,                                               \
            .prop1 = prop_,                                              \
            .type_info = { &buffer_case_info },                          \
            .trials = trials_,                                           \
            .seed = theft_seed_of_time(),                                \
        };                                                               \
        enum theft_run_res res = theft_run(&cfg);                        \
        printf("  [%s] %s\n",                                           \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                           \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("malloc_buffer baseline property-based tests:\n");
    RUN_PROP("allocates_exact_byte_count", prop_allocates_exact_byte_count, 500);
    RUN_PROP("returned_range_is_writable", prop_returned_range_is_writable, 500);
    RUN_PROP("each_call_returns_independent_storage", prop_each_call_returns_independent_storage, 500);
    RUN_PROP("boundary_lengths_are_supported", prop_boundary_lengths_are_supported, 300);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
