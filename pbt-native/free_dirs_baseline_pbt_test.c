/*
 * Property-based tests for free_dirs() in
 * eval/extent/baseline/util.c
 *
 * Uses theft (PBT framework for C): https://github.com/silentbicycle/theft
 *
 * Oracle: Algebraic - deallocation lifecycle invariant.
 * Spec evidence: sysspec/specfs/util/free_dirs.spec says dirname is a
 * NULL-terminated array of dynamically allocated strings; free_dirs must
 * deallocate each non-NULL element before the first NULL terminator and must
 * not free the dirname array itself.
 * Stronger considered:
 *   - State Machine: rejected, free_dirs is a single-shot destructor with no
 *     observable post-state except allocator events.
 *   - Differential: rejected, there is no independent implementation that can
 *     observe this allocator-level contract.
 *   - Round-trip: rejected, deallocation has no inverse operation.
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PREFIX_COMPONENTS 16U
#define MAX_TRAILING_COMPONENTS 4U
#define MAX_COMPONENT_LEN 32U
#define MAX_FREE_EVENTS (MAX_PREFIX_COMPONENTS + MAX_TRAILING_COMPONENTS + 2U)

struct free_dirs_case {
    size_t len;
    size_t trailing_count;
    char names[MAX_PREFIX_COMPONENTS + MAX_TRAILING_COMPONENTS][MAX_COMPONENT_LEN + 1U];
};

struct runtime_dirs {
    char **dirs;
    char *prefix[MAX_PREFIX_COMPONENTS];
    char *trailing[MAX_TRAILING_COMPONENTS];
    size_t len;
    size_t trailing_count;
};

static int tracker_enabled;
static const void *forbidden_array;
static const void *expected_prefix[MAX_PREFIX_COMPONENTS];
static const void *forbidden_trailing[MAX_TRAILING_COMPONENTS];
static size_t expected_prefix_count;
static size_t forbidden_trailing_count;
static unsigned observed_array_frees;
static unsigned observed_prefix_frees[MAX_PREFIX_COMPONENTS];
static unsigned observed_trailing_frees[MAX_TRAILING_COMPONENTS];
static unsigned observed_unknown_frees;
static size_t event_count;
static size_t prefix_event_index[MAX_PREFIX_COMPONENTS];

static void reset_tracker(void)
{
    tracker_enabled = 0;
    forbidden_array = NULL;
    expected_prefix_count = 0U;
    forbidden_trailing_count = 0U;
    observed_array_frees = 0U;
    observed_unknown_frees = 0U;
    event_count = 0U;
    memset(expected_prefix, 0, sizeof(expected_prefix));
    memset(forbidden_trailing, 0, sizeof(forbidden_trailing));
    memset(observed_prefix_frees, 0, sizeof(observed_prefix_frees));
    memset(observed_trailing_frees, 0, sizeof(observed_trailing_frees));
    for (size_t i = 0U; i < MAX_PREFIX_COMPONENTS; i++) {
        prefix_event_index[i] = (size_t)-1;
    }
}

static void record_event(void)
{
    event_count++;
}

void tracked_free(void *ptr)
{
    if (tracker_enabled && ptr != NULL) {
        int matched = 0;

        if (ptr == forbidden_array) {
            observed_array_frees++;
            record_event();
            matched = 1;
        }

        for (size_t i = 0U; !matched && i < expected_prefix_count; i++) {
            if (ptr == expected_prefix[i]) {
                observed_prefix_frees[i]++;
                prefix_event_index[i] = event_count;
                record_event();
                matched = 1;
            }
        }

        for (size_t i = 0U; !matched && i < forbidden_trailing_count; i++) {
            if (ptr == forbidden_trailing[i]) {
                observed_trailing_frees[i]++;
                record_event();
                matched = 1;
            }
        }

        if (!matched) {
            observed_unknown_frees++;
            record_event();
        }
    }

    free(ptr);
}

#define free tracked_free
#include "../eval/extent/baseline/util.c"
#undef free

void brels(struct indextb *tb)
{
    (void)tb;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static char draw_name_char(struct theft *t)
{
    static const unsigned char alphabet[] = {
        'a', 'z', 'A', 'Z', '0', '9', '_', '-', '.', '/', ' ', '\t',
        0x01U, 0x7fU, 0x80U, 0xffU,
    };
    return (char)alphabet[bounded_choice(t, (unsigned)(sizeof(alphabet) / sizeof(alphabet[0])))] ;
}

static size_t draw_name_len(struct theft *t)
{
    switch (theft_random_choice(t, 8U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return MAX_COMPONENT_LEN;
    default:
        return (size_t)bounded_choice(t, MAX_COMPONENT_LEN + 1U);
    }
}

static size_t draw_prefix_len(struct theft *t)
{
    switch (theft_random_choice(t, 10U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return MAX_PREFIX_COMPONENTS;
    default:
        return (size_t)bounded_choice(t, MAX_PREFIX_COMPONENTS + 1U);
    }
}

static enum theft_alloc_res free_dirs_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct free_dirs_case *fc = calloc(1U, sizeof(*fc));
    if (fc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    fc->len = draw_prefix_len(t);
    fc->trailing_count = (size_t)bounded_choice(t, MAX_TRAILING_COMPONENTS + 1U);

    for (size_t i = 0U; i < MAX_PREFIX_COMPONENTS + MAX_TRAILING_COMPONENTS; i++) {
        size_t len = draw_name_len(t);
        for (size_t j = 0U; j < len; j++) {
            fc->names[i][j] = draw_name_char(t);
        }
        fc->names[i][len] = '\0';
    }

    *instance = fc;
    return THEFT_ALLOC_OK;
}

static void free_dirs_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash free_dirs_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct free_dirs_case *fc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&fc->len, sizeof(fc->len));
    theft_hash_sink(&h, (const uint8_t *)&fc->trailing_count, sizeof(fc->trailing_count));
    theft_hash_sink(&h, (const uint8_t *)fc->names, sizeof(fc->names));
    return theft_hash_done(&h);
}

static void print_escaped(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c < 32U || c > 126U) {
            fprintf(f, "\\x%02x", c);
        } else {
            fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static void free_dirs_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct free_dirs_case *fc = instance;
    fprintf(f, "{len=%zu, trailing_count=%zu, names=[", fc->len, fc->trailing_count);
    for (size_t i = 0U; i < fc->len + fc->trailing_count; i++) {
        if (i > 0U) {
            fprintf(f, ", ");
        }
        print_escaped(f, fc->names[i]);
    }
    fprintf(f, "]}");
}

static struct theft_type_info free_dirs_case_info = {
    .alloc = free_dirs_case_alloc_cb,
    .free = free_dirs_case_free_cb,
    .hash = free_dirs_case_hash_cb,
    .print = free_dirs_case_print_cb,
};

static char *dup_component(const char *src)
{
    size_t len = strlen(src);
    char *copy = malloc(len + 1U);
    if (copy != NULL) {
        memcpy(copy, src, len + 1U);
    }
    return copy;
}

static int build_runtime_dirs(const struct free_dirs_case *fc, struct runtime_dirs *rt)
{
    memset(rt, 0, sizeof(*rt));
    rt->len = fc->len;
    rt->trailing_count = fc->trailing_count;

    rt->dirs = calloc(rt->len + rt->trailing_count + 1U, sizeof(char *));
    if (rt->dirs == NULL) {
        return 0;
    }

    for (size_t i = 0U; i < rt->len; i++) {
        rt->prefix[i] = dup_component(fc->names[i]);
        if (rt->prefix[i] == NULL) {
            return 0;
        }
        rt->dirs[i] = rt->prefix[i];
    }

    rt->dirs[rt->len] = NULL;
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        rt->trailing[i] = dup_component(fc->names[MAX_PREFIX_COMPONENTS + i]);
        if (rt->trailing[i] == NULL) {
            return 0;
        }
        rt->dirs[rt->len + 1U + i] = rt->trailing[i];
    }

    return 1;
}

static void start_tracking(const struct runtime_dirs *rt)
{
    reset_tracker();
    forbidden_array = rt->dirs;
    expected_prefix_count = rt->len;
    forbidden_trailing_count = rt->trailing_count;
    for (size_t i = 0U; i < rt->len; i++) {
        expected_prefix[i] = rt->prefix[i];
    }
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        forbidden_trailing[i] = rt->trailing[i];
    }
    tracker_enabled = 1;
}

static void cleanup_remaining(struct runtime_dirs *rt)
{
    tracker_enabled = 0;

    for (size_t i = 0U; i < rt->len; i++) {
        if (rt->prefix[i] != NULL && observed_prefix_frees[i] == 0U) {
            free(rt->prefix[i]);
        }
    }
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        if (rt->trailing[i] != NULL && observed_trailing_frees[i] == 0U) {
            free(rt->trailing[i]);
        }
    }
    if (rt->dirs != NULL && observed_array_frees == 0U) {
        free(rt->dirs);
    }
}

static int all_prefix_freed_once(const struct runtime_dirs *rt)
{
    if (observed_array_frees != 0U || observed_unknown_frees != 0U) {
        return 0;
    }
    for (size_t i = 0U; i < rt->len; i++) {
        if (observed_prefix_frees[i] != 1U) {
            return 0;
        }
    }
    return 1;
}

static int no_trailing_frees(const struct runtime_dirs *rt)
{
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        if (observed_trailing_frees[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_frees_prefix_once_and_stops_at_null(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_dirs_case *fc = arg1;
    struct runtime_dirs rt;
    if (!build_runtime_dirs(fc, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    start_tracking(&rt);
    free_dirs(rt.dirs);

    int ok = all_prefix_freed_once(&rt) && no_trailing_frees(&rt);
    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_prefix_frees_follow_input_order(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_dirs_case *fc = arg1;
    struct runtime_dirs rt;
    if (!build_runtime_dirs(fc, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    start_tracking(&rt);
    free_dirs(rt.dirs);

    int ok = all_prefix_freed_once(&rt) && no_trailing_frees(&rt);
    for (size_t i = 0U; ok && i + 1U < rt.len; i++) {
        ok = prefix_event_index[i] < prefix_event_index[i + 1U];
    }

    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_list_is_noop_and_keeps_array(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_dirs_case *fc = arg1;
    struct runtime_dirs rt;
    if (!build_runtime_dirs(fc, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    rt.len = 0U;
    rt.dirs[0] = NULL;

    start_tracking(&rt);
    free_dirs(rt.dirs);

    int ok = observed_array_frees == 0U && observed_unknown_frees == 0U &&
             event_count == 0U && no_trailing_frees(&rt);
    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_does_not_free_dirname_array(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_dirs_case *fc = arg1;
    struct runtime_dirs rt;
    if (!build_runtime_dirs(fc, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    start_tracking(&rt);
    free_dirs(rt.dirs);

    int ok = observed_array_frees == 0U;
    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                         \
    do {                                                                         \
        struct theft_run_config cfg = {                                          \
            .name = name_,                                                       \
            .prop1 = prop_,                                                      \
            .type_info = { &free_dirs_case_info },                               \
            .trials = trials_,                                                   \
            .seed = theft_seed_of_time(),                                        \
        };                                                                       \
        enum theft_run_res res = theft_run(&cfg);                                \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) {                                             \
            failures++;                                                          \
        }                                                                        \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("free_dirs extent baseline property-based tests:\n");
    RUN_PROP("frees_prefix_once_and_stops_at_null", prop_frees_prefix_once_and_stops_at_null, 500);
    RUN_PROP("prefix_frees_follow_input_order", prop_prefix_frees_follow_input_order, 500);
    RUN_PROP("empty_list_is_noop_and_keeps_array", prop_empty_list_is_noop_and_keeps_array, 300);
    RUN_PROP("does_not_free_dirname_array", prop_does_not_free_dirname_array, 300);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
