/*
 * Theft property-based tests for malloc_path() in
 * eval/delay_alloc/optimization/util.c
 *
 * Oracle: Algebraic allocator invariant. For every requested pointer-array
 * length len, malloc_path(len) must allocate exactly len * sizeof(char*) bytes
 * and zero exactly that many bytes so the returned vector is ready for use as a
 * path slot array.
 * Stronger considered:
 *   - State Machine: rejected, malloc_path is a single allocation helper with
 *     no observable state.
 *   - Differential: rejected, there is no independent implementation beyond
 *     the malloc/memset contract.
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ALLOC_PATH_LEN 256U
#define CANARY_SIZE 32U
#define CANARY_BYTE 0xa5U

static void *(*real_malloc_fn)(size_t) = malloc;
static void (*real_free_fn)(void *) = free;
static void *(*real_memset_fn)(void *, int, size_t) = memset;

struct len_case {
    unsigned len;
};

struct tracked_alloc {
    unsigned char *raw;
    void *user;
    size_t requested;
};

static struct tracked_alloc last_alloc;
static unsigned malloc_calls;
static unsigned memset_calls;
static void *last_memset_ptr;
static int last_memset_value;
static size_t last_memset_len;
static int allocation_overflowed;
static int tracker_enabled;

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res len_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct len_case *lc = calloc(1U, sizeof(*lc));
    if (lc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    switch (theft_random_choice(t, 10U)) {
    case 0:
        lc->len = 0U;
        break;
    case 1:
        lc->len = 1U;
        break;
    case 2:
        lc->len = 2U;
        break;
    case 3:
        lc->len = MAX_ALLOC_PATH_LEN;
        break;
    default:
        lc->len = bounded_choice(t, MAX_ALLOC_PATH_LEN + 1U);
        break;
    }

    *instance = lc;
    return THEFT_ALLOC_OK;
}

static void len_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash len_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct len_case));
}

static void len_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct len_case *lc = instance;
    fprintf(f, "{len=%u}", lc->len);
}

static struct theft_type_info len_case_info = {
    .alloc = len_case_alloc_cb,
    .free = len_case_free_cb,
    .hash = len_case_hash_cb,
    .print = len_case_print_cb,
};

static void reset_tracker(void)
{
    memset(&last_alloc, 0, sizeof(last_alloc));
    malloc_calls = 0U;
    memset_calls = 0U;
    last_memset_ptr = NULL;
    last_memset_value = -1;
    last_memset_len = 0U;
    allocation_overflowed = 0;
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

static void *tracked_memset(void *ptr, int value, size_t len)
{
    if (!tracker_enabled) {
        return real_memset_fn(ptr, value, len);
    }

    memset_calls++;
    last_memset_ptr = ptr;
    last_memset_value = value;
    last_memset_len = len;

    if (ptr != last_alloc.user || len > last_alloc.requested) {
        allocation_overflowed = 1;
    }
    return real_memset_fn(ptr, value, len);
}

#define malloc tracked_malloc
#define free tracked_free
#define memset tracked_memset
#include "../eval/delay_alloc/optimization/util.c"
#undef memset
#undef free
#undef malloc

void brels(struct indextb *tb)
{
    (void)tb;
}

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

static enum theft_trial_res prop_allocates_exact_byte_count(struct theft *t, void *arg1)
{
    (void)t;
    const struct len_case *lc = arg1;
    size_t requested = (size_t)lc->len * sizeof(char *);

    reset_tracker();
    tracker_enabled = 1;
    char **path = malloc_path(lc->len);
    tracker_enabled = 0;

    int ok = path != NULL &&
             path == (char **)last_alloc.user &&
             malloc_calls == 1U &&
             last_alloc.requested == requested &&
             allocation_overflowed == 0 &&
             canaries_intact();

    tracked_free(path);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_initializes_all_slots_to_null(struct theft *t, void *arg1)
{
    (void)t;
    const struct len_case *lc = arg1;
    size_t requested = (size_t)lc->len * sizeof(char *);

    reset_tracker();
    tracker_enabled = 1;
    char **path = malloc_path(lc->len);
    tracker_enabled = 0;

    int ok = path != NULL;
    for (unsigned i = 0U; ok && i < lc->len; i++) {
        ok = path[i] == NULL;
    }
    ok = ok &&
         memset_calls == 1U &&
         last_memset_ptr == path &&
         last_memset_value == 0 &&
         last_memset_len == requested &&
         allocation_overflowed == 0 &&
         canaries_intact();

    tracked_free(path);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_boundary_lengths_are_supported(struct theft *t, void *arg1)
{
    (void)t;
    const struct len_case *lc = arg1;
    if (!(lc->len == 0U || lc->len == 1U || lc->len == 2U || lc->len == MAX_ALLOC_PATH_LEN)) {
        return THEFT_TRIAL_SKIP;
    }

    reset_tracker();
    tracker_enabled = 1;
    char **path = malloc_path(lc->len);
    tracker_enabled = 0;

    int ok = path != NULL;
    for (unsigned i = 0U; ok && i < lc->len; i++) {
        ok = path[i] == NULL;
    }

    tracked_free(path);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                        \
    do {                                                                       \
        struct theft_run_config cfg = {                                        \
            .name = name_,                                                     \
            .prop1 = prop_,                                                    \
            .type_info = { &len_case_info },                                   \
            .trials = trials_,                                                 \
            .seed = theft_seed_of_time(),                                      \
        };                                                                     \
        enum theft_run_res res = theft_run(&cfg);                              \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) {                                           \
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("malloc_path delay_alloc property-based tests:\n");
    RUN_PROP("allocates_exact_byte_count", prop_allocates_exact_byte_count, 500);
    RUN_PROP("initializes_all_slots_to_null", prop_initializes_all_slots_to_null, 500);
    RUN_PROP("boundary_lengths_are_supported", prop_boundary_lengths_are_supported, 200);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
