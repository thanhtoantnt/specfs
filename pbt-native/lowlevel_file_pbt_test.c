/*
 * Property-based tests for file_write() in
 * eval/extent/optimization/lowlevel_file.c
 *
 * The tests focus on the observable contract around page-granularity writes:
 * exact round-tripping, no corruption outside the written span, zero-filling
 * when data == NULL, and the required null-pointer safety contract.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_WRITE_BYTES 1024U
#define MAX_EXTENT_PAGES 3U

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_WRITE_BYTES];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    wc->start_page = bounded_choice(t, 8U);
    wc->page_count = 1U + bounded_choice(t, MAX_EXTENT_PAGES);

    switch (theft_random_choice(t, 4)) {
    case 0:
        wc->page_off = 0U;
        break;
    case 1:
        wc->page_off = PG_SIZE - 1U;
        break;
    case 2:
        wc->page_off = PG_SIZE - 8U;
        break;
    default:
        wc->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = wc->page_count * PG_SIZE - wc->page_off;
    if (span_bytes == 0U) span_bytes = 1U;
    unsigned max_len = span_bytes < MAX_WRITE_BYTES ? span_bytes : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256);
    }

    *instance = wc;
    return THEFT_ALLOC_OK;
}

static void write_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash write_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct write_case));
}

static void write_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct write_case *wc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u}",
            wc->start_page, wc->page_count, wc->page_off, wc->len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

unsigned char *malloc_page(void)
{
    unsigned char *data = malloc(PG_SIZE);
    if (data != NULL) memset(data, 0, PG_SIZE);
    return data;
}

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
}

static struct Extent *make_extent(unsigned start_page, unsigned page_count, unsigned char fill)
{
    struct Extent *ext = malloc(sizeof(*ext));
    if (ext == NULL) return NULL;

    ext->start_page = start_page;
    ext->length = page_count;
    ext->data = malloc(page_count * PG_SIZE);
    if (ext->data == NULL) {
        free(ext);
        return NULL;
    }
    memset(ext->data, fill, page_count * PG_SIZE);
    ext->next = NULL;
    return ext;
}

static void free_extents(struct inode *node)
{
    struct Extent *cur = node->extents;
    while (cur != NULL) {
        struct Extent *next = cur->next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    node->extents = NULL;
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static unsigned case_extent_bytes(const struct write_case *wc)
{
    return wc->page_count * PG_SIZE;
}

static enum theft_trial_res prop_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);

    file_write(&node, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len == 0U ? 1U : wc->len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, wc->len == 0U ? 1U : wc->len);
    file_read(&node, offset, wc->len, (char *)out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) {
        fprintf(stderr, "roundtrip mismatch: offset=%u len=%u\n", offset, wc->len);
    }

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_write_preserves_unrelated_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned extent_bytes = case_extent_bytes(wc);
    unsigned offset = case_offset(wc);
    unsigned char fill = 0xA5;

    node.extents = make_extent(wc->start_page, wc->page_count, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, offset, wc->len, (const char *)wc->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(snapshot, 0, extent_bytes);
    file_read(&node, wc->start_page * PG_SIZE, extent_bytes, (char *)snapshot);

    unsigned prefix = wc->page_off;
    unsigned suffix = extent_bytes - prefix - wc->len;
    int ok = 1;

    for (unsigned i = 0; i < prefix; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "prefix corrupted at %u (got %u)\n", i, snapshot[i]);
            ok = 0;
            break;
        }
    }
    if (ok && memcmp(snapshot + prefix, wc->data, wc->len) != 0) {
        fprintf(stderr, "written span mismatch at offset=%u len=%u\n", offset, wc->len);
        ok = 0;
    }
    for (unsigned i = 0; ok && i < suffix; i++) {
        unsigned idx = prefix + wc->len + i;
        if (snapshot[idx] != fill) {
            fprintf(stderr, "suffix corrupted at %u (got %u)\n", idx, snapshot[idx]);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_data_zero_fills(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);

    file_write(&node, offset, wc->len, NULL);

    unsigned char *out = malloc(wc->len == 0U ? 1U : wc->len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, wc->len == 0U ? 1U : wc->len);
    file_read(&node, offset, wc->len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "null-fill mismatch at %u (got %u)\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_inode_is_safe(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return THEFT_TRIAL_ERROR;
    }

    if (pid == 0) {
        file_write(NULL, 0U, 1U, (const char *)wc->data);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return THEFT_TRIAL_ERROR;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "null inode crashed or returned non-zero: status=%d\n", status);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &write_case_info },                          \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("file_write property-based tests:\n");
    RUN_PROP("roundtrip_matches_input", prop_roundtrip_matches_input, 300);
    RUN_PROP("write_preserves_unrelated_bytes", prop_write_preserves_unrelated_bytes, 300);
    RUN_PROP("null_data_zero_fills", prop_null_data_zero_fills, 200);
    RUN_PROP("null_inode_is_safe", prop_null_inode_is_safe, 1);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
