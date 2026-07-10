/*
 * Property-based tests for free_path() in
 * eval/extent/baseline/util.c
 *
 * Oracle: Algebraic - deallocation lifecycle invariant from the free_path spec:
 * for any NULL-terminated heap path array, free_path frees every component
 * before the first NULL exactly once and frees the array exactly once.
 * Stronger considered:
 *   - State Machine: rejected, free_path is a single-shot destructor with no
 *     observable post-state except the deallocation events.
 *   - Differential: rejected, there is no independent implementation that can
 *     observe the allocator-level contract.
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH_COMPONENTS 16U
#define MAX_TRAILING_COMPONENTS 4U
#define MAX_COMPONENT_LEN 32U
#define MAX_FREE_EVENTS (MAX_PATH_COMPONENTS + MAX_TRAILING_COMPONENTS + 2U)
#define ARRAY_EVENT_KIND 1U
#define COMPONENT_EVENT_KIND 2U
#define TRAILING_EVENT_KIND 3U
#define UNKNOWN_EVENT_KIND 4U

struct free_path_case {
    size_t len;
    size_t trailing_count;
    char components[MAX_PATH_COMPONENTS + MAX_TRAILING_COMPONENTS][MAX_COMPONENT_LEN + 1U];
};

struct runtime_path {
    char **path;
    char *components[MAX_PATH_COMPONENTS];
    char *trailing[MAX_TRAILING_COMPONENTS];
    size_t len;
    size_t trailing_count;
};

static int tracker_enabled;
static const void *expected_array;
static const void *expected_components[MAX_PATH_COMPONENTS];
static const void *forbidden_trailing[MAX_TRAILING_COMPONENTS];
static size_t expected_component_count;
static size_t forbidden_trailing_count;
static unsigned observed_array_frees;
static unsigned observed_component_frees[MAX_PATH_COMPONENTS];
static unsigned observed_trailing_frees[MAX_TRAILING_COMPONENTS];
static unsigned observed_unknown_frees;
static unsigned event_kinds[MAX_FREE_EVENTS];
static size_t event_count;
static size_t array_event_index;
static size_t component_event_index[MAX_PATH_COMPONENTS];

static void reset_tracker(void)
{
    tracker_enabled = 0;
    expected_array = NULL;
    expected_component_count = 0U;
    forbidden_trailing_count = 0U;
    observed_array_frees = 0U;
    observed_unknown_frees = 0U;
    event_count = 0U;
    array_event_index = (size_t)-1;
    memset(expected_components, 0, sizeof(expected_components));
    memset(forbidden_trailing, 0, sizeof(forbidden_trailing));
    memset(observed_component_frees, 0, sizeof(observed_component_frees));
    memset(observed_trailing_frees, 0, sizeof(observed_trailing_frees));
    memset(event_kinds, 0, sizeof(event_kinds));
    for (size_t i = 0U; i < MAX_PATH_COMPONENTS; i++) {
        component_event_index[i] = (size_t)-1;
    }
}

static void record_event(unsigned kind)
{
    if (event_count < MAX_FREE_EVENTS) {
        event_kinds[event_count] = kind;
    }
    event_count++;
}

void tracked_free(void *ptr)
{
    if (tracker_enabled && ptr != NULL) {
        int matched = 0;

        if (ptr == expected_array) {
            observed_array_frees++;
            array_event_index = event_count;
            record_event(ARRAY_EVENT_KIND);
            matched = 1;
        }

        for (size_t i = 0U; !matched && i < expected_component_count; i++) {
            if (ptr == expected_components[i]) {
                observed_component_frees[i]++;
                component_event_index[i] = event_count;
                record_event(COMPONENT_EVENT_KIND);
                matched = 1;
            }
        }

        for (size_t i = 0U; !matched && i < forbidden_trailing_count; i++) {
            if (ptr == forbidden_trailing[i]) {
                observed_trailing_frees[i]++;
                record_event(TRAILING_EVENT_KIND);
                matched = 1;
            }
        }

        if (!matched) {
            observed_unknown_frees++;
            record_event(UNKNOWN_EVENT_KIND);
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

static char draw_component_byte(struct theft *t)
{
    static const unsigned char alphabet[] = {
        'a', 'z', 'A', 'Z', '0', '9', '_', '-', '.', '/', ' ', '\t',
        0x01U, 0x7fU, 0x80U, 0xffU,
    };
    return (char)alphabet[bounded_choice(t, (unsigned)(sizeof(alphabet) / sizeof(alphabet[0])))];
}

static size_t draw_component_len(struct theft *t)
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

static size_t draw_path_len(struct theft *t)
{
    switch (theft_random_choice(t, 10U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return MAX_PATH_COMPONENTS;
    default:
        return (size_t)bounded_choice(t, MAX_PATH_COMPONENTS + 1U);
    }
}

static enum theft_alloc_res free_path_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct free_path_case *fc = calloc(1U, sizeof(*fc));
    if (fc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    fc->len = draw_path_len(t);
    fc->trailing_count = (size_t)bounded_choice(t, MAX_TRAILING_COMPONENTS + 1U);

    for (size_t i = 0U; i < MAX_PATH_COMPONENTS + MAX_TRAILING_COMPONENTS; i++) {
        size_t len = draw_component_len(t);
        for (size_t j = 0U; j < len; j++) {
            fc->components[i][j] = draw_component_byte(t);
        }
        fc->components[i][len] = '\0';
    }

    *instance = fc;
    return THEFT_ALLOC_OK;
}

static void free_path_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash free_path_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct free_path_case *fc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&fc->len, sizeof(fc->len));
    theft_hash_sink(&h, (const uint8_t *)&fc->trailing_count, sizeof(fc->trailing_count));
    theft_hash_sink(&h, (const uint8_t *)fc->components, sizeof(fc->components));
    return theft_hash_done(&h);
}

static void print_escaped(FILE *f, const char *s)
{
    for (size_t i = 0U; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32U && c <= 126U && c != '"' && c != '\\') {
            fputc((int)c, f);
        } else {
            fprintf(f, "\\x%02x", c);
        }
    }
}

static void free_path_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct free_path_case *fc = instance;
    fprintf(f, "{len=%zu, trailing_count=%zu, components=[", fc->len, fc->trailing_count);
    for (size_t i = 0U; i < fc->len; i++) {
        fprintf(f, "%s\"", i == 0U ? "" : ", ");
        print_escaped(f, fc->components[i]);
        fprintf(f, "\"");
    }
    fprintf(f, "]}");
}

static struct theft_type_info free_path_case_info = {
    .alloc = free_path_case_alloc_cb,
    .free = free_path_case_free_cb,
    .hash = free_path_case_hash_cb,
    .print = free_path_case_print_cb,
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

static int build_runtime_path(const struct free_path_case *fc, size_t len_override, struct runtime_path *rt)
{
    memset(rt, 0, sizeof(*rt));
    rt->len = len_override;
    rt->trailing_count = fc->trailing_count;

    rt->path = calloc(rt->len + rt->trailing_count + 1U, sizeof(char *));
    if (rt->path == NULL) {
        return 0;
    }

    for (size_t i = 0U; i < rt->len; i++) {
        rt->components[i] = dup_component(fc->components[i]);
        if (rt->components[i] == NULL) {
            return 0;
        }
        rt->path[i] = rt->components[i];
    }

    rt->path[rt->len] = NULL;
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        rt->trailing[i] = dup_component(fc->components[MAX_PATH_COMPONENTS + i]);
        if (rt->trailing[i] == NULL) {
            return 0;
        }
        rt->path[rt->len + 1U + i] = rt->trailing[i];
    }

    return 1;
}

static void start_tracking(const struct runtime_path *rt)
{
    reset_tracker();
    expected_array = rt->path;
    expected_component_count = rt->len;
    forbidden_trailing_count = rt->trailing_count;
    for (size_t i = 0U; i < rt->len; i++) {
        expected_components[i] = rt->components[i];
    }
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        forbidden_trailing[i] = rt->trailing[i];
    }
    tracker_enabled = 1;
}

static void cleanup_remaining(struct runtime_path *rt)
{
    tracker_enabled = 0;

    for (size_t i = 0U; i < rt->len; i++) {
        if (rt->components[i] != NULL && observed_component_frees[i] == 0U) {
            free(rt->components[i]);
        }
    }
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        if (rt->trailing[i] != NULL && observed_trailing_frees[i] == 0U) {
            free(rt->trailing[i]);
        }
    }
    if (rt->path != NULL && observed_array_frees == 0U) {
        free(rt->path);
    }
}

static int all_owned_pointers_freed_once(const struct runtime_path *rt)
{
    if (observed_array_frees != 1U || observed_unknown_frees != 0U) {
        return 0;
    }
    for (size_t i = 0U; i < rt->len; i++) {
        if (observed_component_frees[i] != 1U) {
            return 0;
        }
    }
    return 1;
}

static int no_trailing_pointers_freed(const struct runtime_path *rt)
{
    for (size_t i = 0U; i < rt->trailing_count; i++) {
        if (observed_trailing_frees[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_frees_components_and_array_once(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_path_case *fc = arg1;
    struct runtime_path rt;
    if (!build_runtime_path(fc, fc->len, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    start_tracking(&rt);
    free_path(rt.path);

    int ok = all_owned_pointers_freed_once(&rt) && no_trailing_pointers_freed(&rt);
    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_stops_at_first_null_terminator(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_path_case *fc = arg1;
    if (fc->trailing_count == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    struct runtime_path rt;
    if (!build_runtime_path(fc, fc->len, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    char trailing_before[MAX_TRAILING_COMPONENTS][MAX_COMPONENT_LEN + 1U];
    for (size_t i = 0U; i < rt.trailing_count; i++) {
        memcpy(trailing_before[i], rt.trailing[i], MAX_COMPONENT_LEN + 1U);
    }

    start_tracking(&rt);
    free_path(rt.path);

    int ok = no_trailing_pointers_freed(&rt);
    for (size_t i = 0U; ok && i < rt.trailing_count; i++) {
        ok = strcmp(rt.trailing[i], trailing_before[i]) == 0;
    }

    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_path_frees_only_array(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_path_case *fc = arg1;
    struct runtime_path rt;
    if (!build_runtime_path(fc, 0U, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    start_tracking(&rt);
    free_path(rt.path);

    int ok = observed_array_frees == 1U &&
             observed_unknown_frees == 0U &&
             no_trailing_pointers_freed(&rt) &&
             event_count == 1U &&
             event_kinds[0] == ARRAY_EVENT_KIND;
    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_components_freed_before_array(struct theft *t, void *arg1)
{
    (void)t;
    const struct free_path_case *fc = arg1;
    struct runtime_path rt;
    if (!build_runtime_path(fc, fc->len, &rt)) {
        cleanup_remaining(&rt);
        return THEFT_TRIAL_ERROR;
    }

    start_tracking(&rt);
    free_path(rt.path);

    int ok = all_owned_pointers_freed_once(&rt) &&
             no_trailing_pointers_freed(&rt) &&
             array_event_index != (size_t)-1;
    for (size_t i = 0U; ok && i < rt.len; i++) {
        ok = component_event_index[i] != (size_t)-1 && component_event_index[i] < array_event_index;
    }

    cleanup_remaining(&rt);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                     \
        struct theft_run_config cfg = {                                      \
            .name = name_,                                                   \
            .prop1 = prop_,                                                  \
            .type_info = { &free_path_case_info },                           \
            .trials = trials_,                                               \
            .seed = theft_seed_of_time(),                                    \
        };                                                                   \
        enum theft_run_res res = theft_run(&cfg);                            \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) {                                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("free_path extent baseline property-based tests:\n");
    RUN_PROP("frees_components_and_array_once", prop_frees_components_and_array_once, 500);
    RUN_PROP("stops_at_first_null_terminator", prop_stops_at_first_null_terminator, 500);
    RUN_PROP("empty_path_frees_only_array", prop_empty_path_frees_only_array, 500);
    RUN_PROP("components_freed_before_array", prop_components_freed_before_array, 500);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
