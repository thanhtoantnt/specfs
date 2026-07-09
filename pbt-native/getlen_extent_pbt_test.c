/*
 * Property-based tests for getlen() in
 * eval/extent/optimization/path_handling.c
 *
 * Oracle: algebraic/reference invariant from the path_handling contract:
 * getlen(path) returns the exact number of non-NULL path components before
 * the first NULL terminator. Stronger state-machine testing is unnecessary
 * because getlen is a pure read-only query over a NULL-terminated array.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path_handling.h"

#define MAX_PATH_COMPONENTS 16U
#define MAX_COMPONENT_LEN 12U

struct getlen_case {
    size_t first_null;
    char storage[MAX_PATH_COMPONENTS][MAX_COMPONENT_LEN + 1U];
    char *path[MAX_PATH_COMPONENTS + 1U];
};

void lock(struct inode *node) { (void)node; }
void unlock(struct inode *node) { (void)node; }
struct inode *find(struct dirtb *dir, char *name)
{
    (void)dir;
    (void)name;
    return NULL;
}

char **malloc_path(unsigned len)
{
    return calloc((size_t)len + 1U, sizeof(char *));
}

char *malloc_string(const char *name)
{
    size_t len = strlen(name);
    char *copy = malloc(len + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, name, len + 1U);
    return copy;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static void fill_component(struct theft *t, char out[MAX_COMPONENT_LEN + 1U])
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    unsigned len = bounded_choice(t, MAX_COMPONENT_LEN + 1U);

    for (unsigned i = 0U; i < len; i++) {
        out[i] = alphabet[bounded_choice(t, (unsigned)(sizeof(alphabet) - 1U))];
    }
    out[len] = '\0';
}

static enum theft_alloc_res getlen_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct getlen_case *tc = calloc(1U, sizeof(*tc));
    if (tc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    tc->first_null = bounded_choice(t, MAX_PATH_COMPONENTS + 1U);
    for (size_t i = 0U; i < MAX_PATH_COMPONENTS; i++) {
        fill_component(t, tc->storage[i]);
        tc->path[i] = tc->storage[i];
    }
    tc->path[tc->first_null] = NULL;
    tc->path[MAX_PATH_COMPONENTS] = NULL;

    *instance = tc;
    return THEFT_ALLOC_OK;
}

static void getlen_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash getlen_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct getlen_case *tc = instance;
    theft_hash hash = theft_hash_onepass((const uint8_t *)&tc->first_null, sizeof(tc->first_null));
    for (size_t i = 0U; i < MAX_PATH_COMPONENTS; i++) {
        hash ^= theft_hash_onepass((const uint8_t *)tc->storage[i], strlen(tc->storage[i]));
    }
    return hash;
}

static void getlen_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct getlen_case *tc = instance;
    fprintf(f, "{first_null=%zu, path=[", tc->first_null);
    for (size_t i = 0U; i < MAX_PATH_COMPONENTS; i++) {
        if (i == tc->first_null) {
            fprintf(f, "%sNULL", i == 0U ? "" : ", ");
        } else {
            fprintf(f, "%s\"%s\"", i == 0U ? "" : ", ", tc->storage[i]);
        }
    }
    fprintf(f, "]}");
}

static struct theft_type_info getlen_case_info = {
    .alloc = getlen_case_alloc_cb,
    .free = getlen_case_free_cb,
    .hash = getlen_case_hash_cb,
    .print = getlen_case_print_cb,
};

static enum theft_trial_res prop_returns_first_null_index(struct theft *t, void *arg1)
{
    (void)t;
    const struct getlen_case *tc = arg1;
    return getlen((char **)tc->path) == (int)tc->first_null
        ? THEFT_TRIAL_PASS
        : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_ignores_entries_after_first_null(struct theft *t, void *arg1)
{
    struct getlen_case *tc = arg1;
    int before = getlen(tc->path);

    for (size_t i = tc->first_null + 1U; i < MAX_PATH_COMPONENTS; i++) {
        fill_component(t, tc->storage[i]);
        tc->path[i] = tc->storage[MAX_PATH_COMPONENTS - 1U - i];
    }
    tc->path[tc->first_null] = NULL;
    tc->path[MAX_PATH_COMPONENTS] = NULL;

    return before == getlen(tc->path) && before == (int)tc->first_null
        ? THEFT_TRIAL_PASS
        : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_does_not_mutate_path(struct theft *t, void *arg1)
{
    (void)t;
    struct getlen_case *tc = arg1;
    char *path_before[MAX_PATH_COMPONENTS + 1U];
    char storage_before[MAX_PATH_COMPONENTS][MAX_COMPONENT_LEN + 1U];

    memcpy(path_before, tc->path, sizeof(path_before));
    memcpy(storage_before, tc->storage, sizeof(storage_before));

    (void)getlen(tc->path);

    return memcmp(path_before, tc->path, sizeof(path_before)) == 0 &&
           memcmp(storage_before, tc->storage, sizeof(storage_before)) == 0
        ? THEFT_TRIAL_PASS
        : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                       \
    do {                                                                       \
        struct theft_run_config cfg = {                                        \
            .name = name_,                                                     \
            .prop1 = prop_,                                                    \
            .type_info = { &getlen_case_info },                                \
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
    printf("getlen extent property-based tests:\n");
    RUN_PROP("returns_first_null_index", prop_returns_first_null_index, 500);
    RUN_PROP("ignores_entries_after_first_null", prop_ignores_entries_after_first_null, 500);
    RUN_PROP("does_not_mutate_path", prop_does_not_mutate_path, 500);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
