/*
 * Theft property-based tests for malloc_page() in
 * eval/extent/baseline/util.c
 *
 * Oracle: Algebraic — allocator invariant / zeroed page contract.
 * Spec evidence: sysspec/specfs/util/malloc_page.spec states the
 * post-condition: "Allocates a new page of memory and returns a pointer to it"
 * and "The allocated memory should be zero-initialized."
 * Stronger considered:
 *   - State Machine: rejected — malloc_page is a single allocation helper
 *     with no lifecycle state beyond caller-owned free().
 *   - Differential: rejected — malloc/calloc are primitives, not independent
 *     project implementations of the page allocator contract.
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

#define CANARY_SIZE 32U
#define CANARY_BYTE 0xa5U

static void *(*real_malloc_fn)(size_t) = malloc;
static void (*real_free_fn)(void *) = free;
static void *(*real_memset_fn)(void *, int, size_t) = memset;

struct page_case {
    unsigned pattern_seed;
};

struct tracked_alloc {
    unsigned char *raw;
    void *user;
    size_t requested;
};

static struct tracked_alloc last_alloc;
static unsigned allocation_calls;
static int tracker_enabled;

static unsigned draw_u32(struct theft *t)
{
    unsigned value = 0U;
    for (unsigned i = 0U; i < sizeof(value); i++) {
        value = (value << 8U) | (unsigned)theft_random_choice(t, 256U);
    }
    return value;
}

static enum theft_alloc_res page_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct page_case *pc = calloc(1U, sizeof(*pc));
    if (pc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    pc->pattern_seed = draw_u32(t);

    *instance = pc;
    return THEFT_ALLOC_OK;
}

static void page_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash page_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct page_case));
}

static void page_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct page_case *pc = instance;
    fprintf(f, "{pattern_seed=%u}", pc->pattern_seed);
}

static struct theft_type_info page_case_info = {
    .alloc = page_case_alloc_cb,
    .free = page_case_free_cb,
    .hash = page_case_hash_cb,
    .print = page_case_print_cb,
};

static void reset_tracker(void)
{
    memset(&last_alloc, 0, sizeof(last_alloc));
    allocation_calls = 0U;
}

static void *tracked_allocate(size_t size, int zero_user)
{
    allocation_calls++;
    size_t total = CANARY_SIZE + size + CANARY_SIZE;
    unsigned char *raw = real_malloc_fn(total == 0U ? 1U : total);
    if (raw == NULL) {
        return NULL;
    }

    real_memset_fn(raw, CANARY_BYTE, total == 0U ? 1U : total);
    last_alloc.raw = raw;
    last_alloc.user = raw + CANARY_SIZE;
    last_alloc.requested = size;
    if (zero_user && size > 0U) {
        real_memset_fn(last_alloc.user, 0, size);
    }
    return last_alloc.user;
}

static void *tracked_malloc(size_t size)
{
    if (!tracker_enabled) {
        return real_malloc_fn(size);
    }
    return tracked_allocate(size, 0);
}

static void *tracked_calloc(size_t count, size_t size)
{
    if (!tracker_enabled) {
        size_t total = count * size;
        void *ptr = real_malloc_fn(total == 0U ? 1U : total);
        if (ptr != NULL && total > 0U) {
            real_memset_fn(ptr, 0, total);
        }
        return ptr;
    }

    if (count != 0U && size > ((size_t)-1) / count) {
        return NULL;
    }
    return tracked_allocate(count * size, 1);
}

static void *tracked_memset(void *ptr, int value, size_t size)
{
    return real_memset_fn(ptr, value, size);
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
#define calloc tracked_calloc
#define memset tracked_memset
#include "../eval/extent/baseline/util.c"
#undef memset
#undef calloc
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

static unsigned char byte_for(const struct page_case *pc, unsigned index, unsigned salt)
{
    unsigned mixed = pc->pattern_seed ^ (index * 1103515245U) ^ salt;
    return (unsigned char)(mixed & 0xffU);
}

static enum theft_trial_res prop_allocates_exactly_one_page(struct theft *t, void *arg1)
{
    (void)t;
    (void)arg1;

    reset_tracker();
    tracker_enabled = 1;
    unsigned char *page = malloc_page();
    tracker_enabled = 0;

    int ok = page != NULL &&
             page == (unsigned char *)last_alloc.user &&
             allocation_calls == 1U &&
             last_alloc.requested == (size_t)PG_SIZE &&
             canaries_intact();
    if (!ok) {
        fprintf(stderr,
                "allocation mismatch: page=%p user=%p calls=%u requested=%zu PG_SIZE=%u\n",
                (void *)page, last_alloc.user, allocation_calls,
                last_alloc.requested, (unsigned)PG_SIZE);
    }

    tracked_free(page);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_page_is_zero_initialized(struct theft *t, void *arg1)
{
    (void)t;
    (void)arg1;

    reset_tracker();
    tracker_enabled = 1;
    unsigned char *page = malloc_page();
    tracker_enabled = 0;
    if (page == NULL) return THEFT_TRIAL_ERROR;

    int ok = canaries_intact();
    for (unsigned i = 0U; ok && i < PG_SIZE; i++) {
        ok = page[i] == 0U;
    }
    if (!ok) {
        fprintf(stderr, "page is not zeroed for all %u bytes\n", (unsigned)PG_SIZE);
    }

    tracked_free(page);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_page_range_is_writable(struct theft *t, void *arg1)
{
    (void)t;
    const struct page_case *pc = arg1;

    reset_tracker();
    tracker_enabled = 1;
    unsigned char *page = malloc_page();
    tracker_enabled = 0;
    if (page == NULL) return THEFT_TRIAL_ERROR;

    for (unsigned i = 0U; i < PG_SIZE; i++) {
        page[i] = byte_for(pc, i, 0x13579bdfU);
    }

    int ok = canaries_intact();
    for (unsigned i = 0U; ok && i < PG_SIZE; i++) {
        ok = page[i] == byte_for(pc, i, 0x13579bdfU);
    }
    if (!ok) {
        fprintf(stderr, "writable range/canary failure for PG_SIZE=%u\n", (unsigned)PG_SIZE);
    }

    tracked_free(page);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_repeated_pages_do_not_alias(struct theft *t, void *arg1)
{
    (void)t;
    const struct page_case *pc = arg1;

    unsigned char *first = malloc_page();
    unsigned char *second = malloc_page();
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return THEFT_TRIAL_ERROR;
    }

    for (unsigned i = 0U; i < PG_SIZE; i++) {
        first[i] = byte_for(pc, i, 0x2468ace0U);
        second[i] = byte_for(pc, i, 0xfdb97531U);
    }

    int ok = first != second;
    for (unsigned i = 0U; ok && i < PG_SIZE; i++) {
        ok = first[i] == byte_for(pc, i, 0x2468ace0U) &&
             second[i] == byte_for(pc, i, 0xfdb97531U);
    }
    if (!ok) {
        fprintf(stderr, "allocated pages alias or corrupt each other\n");
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
            .type_info = { &page_case_info },                            \
            .trials = trials_,                                           \
            .seed = theft_seed_of_time(),                                \
        };                                                               \
        enum theft_run_res res = theft_run(&cfg);                        \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);       \
        if (res != THEFT_RUN_PASS) failures++;                           \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("malloc_page baseline property-based tests:\n");
    RUN_PROP("allocates_exactly_one_page", prop_allocates_exactly_one_page, 500);
    RUN_PROP("page_is_zero_initialized", prop_page_is_zero_initialized, 500);
    RUN_PROP("page_range_is_writable", prop_page_range_is_writable, 500);
    RUN_PROP("repeated_pages_do_not_alias", prop_repeated_pages_do_not_alias, 500);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
