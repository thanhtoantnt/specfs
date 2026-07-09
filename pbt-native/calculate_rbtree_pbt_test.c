#include <theft.h>

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH_COMPONENTS 8
#define MAX_COMPONENT_LEN 8

struct path_pair_instance {
    size_t src_len;
    size_t dst_len;
    char src_storage[MAX_PATH_COMPONENTS][MAX_COMPONENT_LEN + 1];
    char dst_storage[MAX_PATH_COMPONENTS][MAX_COMPONENT_LEN + 1];
    char *src[MAX_PATH_COMPONENTS + 1];
    char *dst[MAX_PATH_COMPONENTS + 1];
};

static int fail_realloc_after = -1;
static int realloc_calls = 0;

static void *tracked_realloc(void *ptr, size_t size);
#define realloc tracked_realloc
#include "../eval/rbtree/optimization/path_handling.c"
#undef realloc

static void *tracked_realloc(void *ptr, size_t size)
{
    if (fail_realloc_after >= 0 && realloc_calls++ >= fail_realloc_after) {
        return NULL;
    }
    return realloc(ptr, size);
}

char **malloc_path(unsigned len)
{
    return calloc((size_t)len + 1, sizeof(char *));
}

char *malloc_string(const char *src)
{
    size_t len = strlen(src);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, src, len + 1);
    return copy;
}

void lock(struct inode *node) { (void)node; }
void unlock(struct inode *node) { (void)node; }
struct inode *find(struct dirtb *dir, char *name)
{
    (void)dir;
    (void)name;
    return NULL;
}

static void fill_component(struct theft *t, char out[MAX_COMPONENT_LEN + 1])
{
    uint64_t len = theft_random_choice(t, MAX_COMPONENT_LEN + 1);
    for (uint64_t i = 0; i < len; i++) {
        out[i] = (char)('a' + theft_random_choice(t, 4));
    }
    out[len] = '\0';
}

static void init_path_views(struct path_pair_instance *pair)
{
    for (size_t i = 0; i < pair->src_len; i++) {
        pair->src[i] = pair->src_storage[i];
    }
    pair->src[pair->src_len] = NULL;
    for (size_t i = 0; i < pair->dst_len; i++) {
        pair->dst[i] = pair->dst_storage[i];
    }
    pair->dst[pair->dst_len] = NULL;
}

static enum theft_alloc_res path_pair_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct path_pair_instance *pair = calloc(1, sizeof(*pair));
    if (pair == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    pair->src_len = theft_random_choice(t, MAX_PATH_COMPONENTS + 1);
    pair->dst_len = theft_random_choice(t, MAX_PATH_COMPONENTS + 1);
    size_t min_len = pair->src_len < pair->dst_len ? pair->src_len : pair->dst_len;
    size_t shared_len = min_len == 0 ? 0 : theft_random_choice(t, min_len + 1);

    for (size_t i = 0; i < shared_len; i++) {
        fill_component(t, pair->src_storage[i]);
        strcpy(pair->dst_storage[i], pair->src_storage[i]);
    }
    for (size_t i = shared_len; i < pair->src_len; i++) {
        fill_component(t, pair->src_storage[i]);
    }
    for (size_t i = shared_len; i < pair->dst_len; i++) {
        fill_component(t, pair->dst_storage[i]);
    }

    init_path_views(pair);
    *instance = pair;
    return THEFT_ALLOC_OK;
}

static void path_pair_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash path_pair_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct path_pair_instance *pair = instance;
    theft_hash hash = theft_hash_onepass((const uint8_t *)&pair->src_len, sizeof(pair->src_len));
    hash ^= theft_hash_onepass((const uint8_t *)&pair->dst_len, sizeof(pair->dst_len));
    for (size_t i = 0; i < pair->src_len; i++) {
        hash ^= theft_hash_onepass((const uint8_t *)pair->src_storage[i], strlen(pair->src_storage[i]));
    }
    for (size_t i = 0; i < pair->dst_len; i++) {
        hash ^= theft_hash_onepass((const uint8_t *)pair->dst_storage[i], strlen(pair->dst_storage[i]));
    }
    return hash;
}

static void path_pair_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct path_pair_instance *pair = instance;
    fprintf(f, "src=[");
    for (size_t i = 0; i < pair->src_len; i++) {
        fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", pair->src_storage[i]);
    }
    fprintf(f, "], dst=[");
    for (size_t i = 0; i < pair->dst_len; i++) {
        fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", pair->dst_storage[i]);
    }
    fprintf(f, "]");
}

static struct theft_type_info path_pair_info = {
    .alloc = path_pair_alloc_cb,
    .free = path_pair_free_cb,
    .hash = path_pair_hash_cb,
    .print = path_pair_print_cb,
};

static size_t reference_common_prefix_len(const struct path_pair_instance *pair)
{
    size_t len = 0;
    while (len < pair->src_len && len < pair->dst_len &&
           strcmp(pair->src_storage[len], pair->dst_storage[len]) == 0) {
        len++;
    }
    return len;
}

static size_t result_len(char **path)
{
    size_t len = 0;
    while (path[len] != NULL) {
        len++;
    }
    return len;
}

static void free_result(char **path)
{
    if (path == NULL) {
        return;
    }
    for (size_t i = 0; path[i] != NULL; i++) {
        free(path[i]);
    }
    free(path);
}

static enum theft_trial_res prop_matches_reference_prefix(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    fail_realloc_after = -1;
    realloc_calls = 0;

    char **result = calculate(pair->src, pair->dst);
    size_t expected_len = reference_common_prefix_len(pair);
    if (result == NULL || result_len(result) != expected_len) {
        free_result(result);
        return THEFT_TRIAL_FAIL;
    }
    for (size_t i = 0; i < expected_len; i++) {
        if (strcmp(result[i], pair->src_storage[i]) != 0) {
            free_result(result);
            return THEFT_TRIAL_FAIL;
        }
    }
    free_result(result);
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_prefix_is_maximal(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    fail_realloc_after = -1;
    realloc_calls = 0;

    char **result = calculate(pair->src, pair->dst);
    if (result == NULL) {
        return THEFT_TRIAL_FAIL;
    }
    size_t len = result_len(result);
    if (len < pair->src_len && len < pair->dst_len &&
        strcmp(pair->src_storage[len], pair->dst_storage[len]) == 0) {
        free_result(result);
        return THEFT_TRIAL_FAIL;
    }
    free_result(result);
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_result_is_null_terminated(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    fail_realloc_after = -1;
    realloc_calls = 0;

    char **result = calculate(pair->src, pair->dst);
    size_t expected_len = reference_common_prefix_len(pair);
    if (result == NULL || result[expected_len] != NULL) {
        free_result(result);
        return THEFT_TRIAL_FAIL;
    }
    free_result(result);
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_result_is_deep_copy(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    fail_realloc_after = -1;
    realloc_calls = 0;

    char **result = calculate(pair->src, pair->dst);
    size_t expected_len = reference_common_prefix_len(pair);
    if (result == NULL) {
        return THEFT_TRIAL_FAIL;
    }
    for (size_t i = 0; i < expected_len; i++) {
        if (result[i] == pair->src[i] || result[i] == pair->dst[i]) {
            free_result(result);
            return THEFT_TRIAL_FAIL;
        }
    }
    if (expected_len > 0) {
        char before[MAX_COMPONENT_LEN + 1];
        strcpy(before, result[0]);
        pair->src_storage[0][0] = pair->src_storage[0][0] == 'z' ? 'y' : 'z';
        if (strcmp(result[0], before) != 0) {
            free_result(result);
            return THEFT_TRIAL_FAIL;
        }
    }
    free_result(result);
    return THEFT_TRIAL_PASS;
}

static sigjmp_buf segv_jmp;
static volatile sig_atomic_t catching_segv = 0;

static void segv_handler(int signo)
{
    (void)signo;
    if (catching_segv) {
        siglongjmp(segv_jmp, 1);
    }
    abort();
}

static enum theft_trial_res prop_realloc_failure_does_not_crash(struct theft *t, void *arg1)
{
    (void)t;
    struct path_pair_instance *pair = arg1;
    struct sigaction action;
    struct sigaction old_action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = segv_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &old_action);

    fail_realloc_after = 0;
    realloc_calls = 0;
    catching_segv = 1;
    if (sigsetjmp(segv_jmp, 1) != 0) {
        catching_segv = 0;
        fail_realloc_after = -1;
        sigaction(SIGSEGV, &old_action, NULL);
        return THEFT_TRIAL_FAIL;
    }

    char **result = calculate(pair->src, pair->dst);
    catching_segv = 0;
    fail_realloc_after = -1;
    sigaction(SIGSEGV, &old_action, NULL);
    free_result(result);
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &path_pair_info },                                 \
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
    printf("calculate rbtree property-based tests:\n");
    RUN_PROP("matches_reference_prefix", prop_matches_reference_prefix, 300);
    RUN_PROP("prefix_is_maximal", prop_prefix_is_maximal, 300);
    RUN_PROP("result_is_null_terminated", prop_result_is_null_terminated, 300);
    RUN_PROP("result_is_deep_copy", prop_result_is_deep_copy, 300);
    RUN_PROP("realloc_failure_does_not_crash", prop_realloc_failure_does_not_crash, 50);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
