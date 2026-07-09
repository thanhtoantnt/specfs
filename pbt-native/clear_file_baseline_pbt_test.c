/*
 * Property-based tests for clear_file() in
 * eval/extent/baseline/lowlevel_file.c
 *
 * Oracle: Algebraic - Invariant (4d) plus boundary negative contract.
 * Stronger considered:
 *   - State Machine (3): rejected because clear_file is a single-call mutation.
 *   - Differential (7): rejected because there is no independent baseline clear implementation.
 *   - Round-trip (4a): rejected because clearing is intentionally lossy.
 * Weaker available: Crash-Only (6).
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

#define MAX_CLEAR_BYTES 1024U
#define MAX_TEST_PAGES 4U
#define MAX_START_PAGE 16U

struct clear_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned oob_tail;
    unsigned oob_extra;
    unsigned char fill;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res clear_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct clear_case *cc = malloc(sizeof(*cc));
    if (cc == NULL) return THEFT_ALLOC_ERROR;

    cc->start_page = bounded_choice(t, MAX_START_PAGE);
    cc->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        cc->page_off = 0U;
        break;
    case 1:
        cc->page_off = 1U;
        break;
    case 2:
        cc->page_off = PG_SIZE - 1U;
        break;
    case 3:
        cc->page_off = PG_SIZE - 16U;
        break;
    case 4:
        cc->page_off = PG_SIZE / 2U;
        break;
    default:
        cc->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span = cc->page_count * PG_SIZE - cc->page_off;
    unsigned max_len = span < MAX_CLEAR_BYTES ? span : MAX_CLEAR_BYTES;
    cc->len = 1U + bounded_choice(t, max_len);
    cc->oob_tail = 1U + bounded_choice(t, 32U);
    cc->oob_extra = 1U + bounded_choice(t, 64U);
    cc->fill = (unsigned char)(1U + bounded_choice(t, 255U));

    *instance = cc;
    return THEFT_ALLOC_OK;
}

static void clear_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash clear_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct clear_case));
}

static void clear_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct clear_case *cc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, "
               "oob_tail=%u, oob_extra=%u, fill=%u}",
            cc->start_page, cc->page_count, cc->page_off, cc->len,
            cc->oob_tail, cc->oob_extra, cc->fill);
}

static struct theft_type_info clear_case_info = {
    .alloc = clear_case_alloc_cb,
    .free = clear_case_free_cb,
    .hash = clear_case_hash_cb,
    .print = clear_case_print_cb,
};

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.file = calloc(1U, sizeof(*node.file));
    return node;
}

static void destroy_inode(struct inode *node)
{
    if (node->file != NULL) {
        for (unsigned i = 0; i < INDEXTB_NUM; i++) {
            free(node->file->index[i]);
        }
        free(node->file);
        node->file = NULL;
    }
}

static int allocate_pages(struct inode *node, unsigned start_page,
                          unsigned page_count, unsigned char fill)
{
    for (unsigned i = 0; i < page_count; i++) {
        unsigned page = start_page + i;
        node->file->index[page] = malloc_page();
        if (node->file->index[page] == NULL) return 0;
        memset(node->file->index[page], fill, PG_SIZE);
    }
    return 1;
}

static unsigned case_offset(const struct clear_case *cc)
{
    return cc->start_page * PG_SIZE + cc->page_off;
}

static unsigned case_extent_bytes(const struct clear_case *cc)
{
    return cc->page_count * PG_SIZE;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    return (offset + len - 1U) / PG_SIZE;
}

static enum theft_trial_res prop_clear_zeros_target_span(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node = make_inode();
    if (node.file == NULL) return THEFT_TRIAL_ERROR;

    unsigned offset = case_offset(cc);
    unsigned extent_bytes = case_extent_bytes(cc);
    if (!allocate_pages(&node, cc->start_page, cc->page_count, cc->fill)) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    clear_file(&node, offset, cc->len);

    unsigned char *after = malloc(extent_bytes);
    if (after == NULL) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(after, 0xCC, extent_bytes);
    file_read(node.file, cc->start_page * PG_SIZE, extent_bytes, (char *)after);

    unsigned start = cc->page_off;
    unsigned end = start + cc->len;
    int ok = 1;
    for (unsigned i = 0; i < extent_bytes; i++) {
        unsigned expected = (i >= start && i < end) ? 0U : cc->fill;
        if (after[i] != expected) {
            fprintf(stderr,
                    "clear span mismatch at byte=%u got=%u expected=%u offset=%u len=%u\n",
                    i, after[i], expected, offset, cc->len);
            ok = 0;
            break;
        }
    }

    free(after);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_clear_allocates_sparse_range(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node = make_inode();
    if (node.file == NULL) return THEFT_TRIAL_ERROR;

    unsigned offset = case_offset(cc);
    clear_file(&node, offset, cc->len);

    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, cc->len);
    for (unsigned page = start_page; page <= end_page; page++) {
        if (node.file->index[page] == NULL) {
            fprintf(stderr, "page %u not allocated after clear_file offset=%u len=%u\n",
                    page, offset, cc->len);
            destroy_inode(&node);
            return THEFT_TRIAL_FAIL;
        }
    }

    unsigned char *out = malloc(cc->len);
    if (out == NULL) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, cc->len);
    file_read(node.file, offset, cc->len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < cc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "sparse clear returned non-zero at byte=%u value=%u\n",
                    i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_clear_zero_len_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node = make_inode();
    if (node.file == NULL) return THEFT_TRIAL_ERROR;

    unsigned offset = case_offset(cc);
    unsigned extent_bytes = case_extent_bytes(cc);
    if (!allocate_pages(&node, cc->start_page, cc->page_count, cc->fill)) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *before = malloc(extent_bytes);
    unsigned char *after = malloc(extent_bytes);
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    file_read(node.file, cc->start_page * PG_SIZE, extent_bytes, (char *)before);
    clear_file(&node, offset, 0U);
    file_read(node.file, cc->start_page * PG_SIZE, extent_bytes, (char *)after);

    int ok = memcmp(before, after, extent_bytes) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length clear modified data offset=%u\n", offset);
    }

    free(before);
    free(after);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static int child_run_out_of_range(unsigned tail, unsigned extra, unsigned char fill)
{
    struct inode node = make_inode();
    if (node.file == NULL) return 3;

    node.file->index[INDEXTB_NUM - 1U] = malloc_page();
    if (node.file->index[INDEXTB_NUM - 1U] == NULL) {
        destroy_inode(&node);
        return 3;
    }
    memset(node.file->index[INDEXTB_NUM - 1U], fill, PG_SIZE);

    unsigned char before[PG_SIZE];
    memcpy(before, node.file->index[INDEXTB_NUM - 1U], sizeof(before));

    clear_file(&node, MAX_FILE_SIZE - tail, tail + extra);

    int unchanged = memcmp(before, node.file->index[INDEXTB_NUM - 1U], sizeof(before)) == 0;
    destroy_inode(&node);
    return unchanged ? 0 : 2;
}

static enum theft_trial_res prop_out_of_range_does_not_crash_or_mutate(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return THEFT_TRIAL_ERROR;
    }

    if (pid == 0) {
        int rc = child_run_out_of_range(cc->oob_tail, cc->oob_extra, cc->fill);
        _exit(rc);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return THEFT_TRIAL_ERROR;
    }

    if (!WIFEXITED(status)) {
        fprintf(stderr,
                "out-of-range clear crashed tail=%u extra=%u status=%d\n",
                cc->oob_tail, cc->oob_extra, status);
        return THEFT_TRIAL_FAIL;
    }
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "out-of-range clear mutated final page or hit setup error tail=%u extra=%u exit=%d\n",
                cc->oob_tail, cc->oob_extra, WEXITSTATUS(status));
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &clear_case_info },                         \
            .trials = trials_,                                         \
            .seed = theft_seed_of_time(),                              \
        };                                                             \
        enum theft_run_res res = theft_run(&cfg);                      \
        printf("  [%s] %s\n",                                        \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);     \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("baseline extent clear_file property-based tests:\n");
    RUN_PROP("zeros_target_span", prop_clear_zeros_target_span, 300);
    RUN_PROP("allocates_sparse_range", prop_clear_allocates_sparse_range, 300);
    RUN_PROP("zero_len_is_noop", prop_clear_zero_len_is_noop, 200);
    RUN_PROP("out_of_range_does_not_crash_or_mutate", prop_out_of_range_does_not_crash_or_mutate, 25);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
