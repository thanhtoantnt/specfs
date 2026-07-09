/*
 * Property-based tests for splitDirs() in
 * eval/extent/optimization/util.c
 *
 * Uses theft (PBT framework for C): https://github.com/silentbicycle/theft
 *
 * Oracle: splitDirs(path, dirname) tokenizes a slash-delimited path into
 * dirname entries. For valid NUL-terminated paths whose non-empty components
 * are shorter than MAX_FILE_LEN and whose count fits MAX_PATH_LEN, it should:
 *   - ignore empty components from leading/trailing/repeated '/';
 *   - allocate/copy every non-empty component into dirname in order;
 *   - terminate the dirname vector with NULL;
 *   - treat the input path as read-only;
 *   - return output strings that do not alias the input buffer.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "util.h"

#define MAX_TEST_COMPONENTS 16
#define MAX_TEST_COMPONENT_LEN 32
#define MAX_TEST_PATH_LEN 640
#define SENTINEL_PTR ((char *)0x1)

struct splitdirs_case {
    size_t count;
    char components[MAX_TEST_COMPONENTS][MAX_TEST_COMPONENT_LEN + 1];
    char path[MAX_TEST_PATH_LEN];
};

struct splitdirs_result {
    char *dirs[MAX_PATH_LEN];
};

static char
random_component_char(struct theft *t)
{
    char c;
    do {
        c = (char)(32 + theft_random_choice(t, 95));
    } while (c == '/');
    return c;
}

static void
append_char(char *buf, size_t *off, char c)
{
    if (*off + 1 < MAX_TEST_PATH_LEN) {
        buf[*off] = c;
        (*off)++;
        buf[*off] = '\0';
    }
}

static void
append_slashes(char *buf, size_t *off, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        append_char(buf, off, '/');
    }
}

static void
append_string(char *buf, size_t *off, const char *s)
{
    while (*s != '\0') {
        append_char(buf, off, *s++);
    }
}

static void
build_noisy_path(struct theft *t, struct splitdirs_case *sp)
{
    size_t off = 0;
    sp->path[0] = '\0';

    if (sp->count == 0) {
        append_slashes(sp->path, &off, theft_random_choice(t, 6));
        return;
    }

    append_slashes(sp->path, &off, theft_random_choice(t, 4));
    for (size_t i = 0; i < sp->count; i++) {
        append_string(sp->path, &off, sp->components[i]);
        if (i + 1 < sp->count) {
            append_slashes(sp->path, &off, 1 + theft_random_choice(t, 4));
        }
    }
    append_slashes(sp->path, &off, theft_random_choice(t, 4));
}

static void
build_canonical_path(const struct splitdirs_case *sp,
                     char out[MAX_TEST_PATH_LEN])
{
    size_t off = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sp->count; i++) {
        if (i > 0) append_char(out, &off, '/');
        append_string(out, &off, sp->components[i]);
    }
}

static enum theft_alloc_res
splitdirs_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct splitdirs_case *sp = malloc(sizeof(*sp));
    if (sp == NULL) return THEFT_ALLOC_ERROR;
    memset(sp, 0, sizeof(*sp));

    sp->count = (size_t)theft_random_choice(t, MAX_TEST_COMPONENTS + 1);
    for (size_t i = 0; i < sp->count; i++) {
        size_t len = 1 + (size_t)theft_random_choice(t, MAX_TEST_COMPONENT_LEN);
        for (size_t j = 0; j < len; j++) {
            sp->components[i][j] = random_component_char(t);
        }
        sp->components[i][len] = '\0';
    }
    build_noisy_path(t, sp);

    *instance = sp;
    return THEFT_ALLOC_OK;
}

static void
splitdirs_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash
splitdirs_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct splitdirs_case *sp = instance;
    return theft_hash_onepass((const uint8_t *)sp->path, strlen(sp->path));
}

static void
print_escaped(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', f);
            fputc(*s, f);
        } else if ((unsigned char)*s < 32 || (unsigned char)*s > 126) {
            fprintf(f, "\\x%02x", (unsigned char)*s);
        } else {
            fputc(*s, f);
        }
    }
    fputc('"', f);
}

static void
splitdirs_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct splitdirs_case *sp = instance;
    fprintf(f, "path=");
    print_escaped(f, sp->path);
    fprintf(f, " components=[");
    for (size_t i = 0; i < sp->count; i++) {
        if (i > 0) fprintf(f, ", ");
        print_escaped(f, sp->components[i]);
    }
    fprintf(f, "]");
}

static struct theft_type_info splitdirs_info = {
    .alloc = splitdirs_alloc_cb,
    .free = splitdirs_free_cb,
    .hash = splitdirs_hash_cb,
    .print = splitdirs_print_cb,
};

static void
init_result(struct splitdirs_result *r)
{
    memset(r->dirs, 0, sizeof(r->dirs));
}

static void
cleanup_result(struct splitdirs_result *r)
{
    free_dirs(r->dirs);
}

static void
run_split(const char *path, struct splitdirs_result *r)
{
    init_result(r);
    splitDirs(path, r->dirs);
}

static int
result_matches_components(const struct splitdirs_result *r,
                          const struct splitdirs_case *sp,
                          const char *context)
{
    for (size_t i = 0; i < sp->count; i++) {
        if (r->dirs[i] == NULL) {
            fprintf(stderr, "%s: dirname[%zu] unexpectedly NULL\n", context, i);
            return 0;
        }
        if (strcmp(r->dirs[i], sp->components[i]) != 0) {
            fprintf(stderr, "%s: dirname[%zu] mismatch: got '%s', expected '%s'\n",
                    context, i, r->dirs[i], sp->components[i]);
            return 0;
        }
    }

    if (r->dirs[sp->count] != NULL) {
        fprintf(stderr, "%s: dirname terminator missing at index %zu\n",
                context, sp->count);
        return 0;
    }

    return 1;
}

static int
results_equal(const struct splitdirs_result *a, const struct splitdirs_result *b,
              const struct splitdirs_case *sp)
{
    for (size_t i = 0; i <= sp->count; i++) {
        if ((a->dirs[i] == NULL) != (b->dirs[i] == NULL)) return 0;
        if (a->dirs[i] != NULL && strcmp(a->dirs[i], b->dirs[i]) != 0) return 0;
    }
    return 1;
}

static enum theft_trial_res
prop_split_matches_components(struct theft *t, void *arg1)
{
    (void)t;
    const struct splitdirs_case *sp = arg1;
    struct splitdirs_result r;
    run_split(sp->path, &r);

    int ok = result_matches_components(&r, sp, "split_matches_components");
    cleanup_result(&r);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_redundant_slashes_equivalent(struct theft *t, void *arg1)
{
    (void)t;
    const struct splitdirs_case *sp = arg1;
    char canonical[MAX_TEST_PATH_LEN];
    struct splitdirs_result noisy;
    struct splitdirs_result plain;

    build_canonical_path(sp, canonical);
    run_split(sp->path, &noisy);
    run_split(canonical, &plain);

    int ok = results_equal(&noisy, &plain, sp);
    if (!ok) {
        fprintf(stderr, "redundant_slashes_equivalent: noisy path ");
        print_escaped(stderr, sp->path);
        fprintf(stderr, " disagrees with canonical ");
        print_escaped(stderr, canonical);
        fprintf(stderr, "\n");
    }

    cleanup_result(&noisy);
    cleanup_result(&plain);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_input_not_modified(struct theft *t, void *arg1)
{
    (void)t;
    const struct splitdirs_case *sp = arg1;
    char mutable_path[MAX_TEST_PATH_LEN];
    char before[MAX_TEST_PATH_LEN];
    struct splitdirs_result r;

    strcpy(mutable_path, sp->path);
    strcpy(before, mutable_path);
    run_split(mutable_path, &r);

    int ok = strcmp(mutable_path, before) == 0;
    if (!ok) {
        fprintf(stderr, "input_not_modified: input changed from ");
        print_escaped(stderr, before);
        fprintf(stderr, " to ");
        print_escaped(stderr, mutable_path);
        fprintf(stderr, "\n");
    }

    cleanup_result(&r);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_outputs_do_not_alias_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct splitdirs_case *sp = arg1;
    char mutable_path[MAX_TEST_PATH_LEN];
    struct splitdirs_result r;

    strcpy(mutable_path, sp->path);
    run_split(mutable_path, &r);
    memset(mutable_path, 'X', strlen(mutable_path));

    int ok = result_matches_components(&r, sp, "outputs_do_not_alias_input");
    cleanup_result(&r);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res
prop_terminates_uninitialized_output(struct theft *t, void *arg1)
{
    (void)t;
    const struct splitdirs_case *sp = arg1;
    char *dirs[MAX_PATH_LEN];
    size_t term = sp->count;

    for (size_t i = 0; i < MAX_PATH_LEN; i++) {
        dirs[i] = SENTINEL_PTR;
    }

    splitDirs(sp->path, dirs);

    int ok = dirs[term] == NULL;
    if (!ok) {
        fprintf(stderr, "terminates_uninitialized_output: path ");
        print_escaped(stderr, sp->path);
        fprintf(stderr, " left dirname[%zu] non-NULL\n", term);
    }

    for (size_t i = 0; i < term; i++) {
        if (dirs[i] != SENTINEL_PTR) free(dirs[i]);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &splitdirs_info },                                 \
            .trials = trials_,                                                \
            .seed = theft_seed_of_time(),                                     \
        };                                                                    \
        enum theft_run_res res = theft_run(&cfg);                             \
        printf("  [%s] %s\n",                                               \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);            \
        if (res != THEFT_RUN_PASS) failures++;                                \
    } while (0)

int
main(void)
{
    int failures = 0;

    printf("splitDirs extent property-based tests:\n");
    RUN_PROP("split_matches_components", prop_split_matches_components, 500);
    RUN_PROP("redundant_slashes_equivalent", prop_redundant_slashes_equivalent, 500);
    RUN_PROP("input_not_modified", prop_input_not_modified, 300);
    RUN_PROP("outputs_do_not_alias_input", prop_outputs_do_not_alias_input, 300);
    RUN_PROP("terminates_uninitialized_output", prop_terminates_uninitialized_output, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
