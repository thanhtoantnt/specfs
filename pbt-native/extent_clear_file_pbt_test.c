/*
 * Property-based tests for clear_file() in
 * eval/extent/optimization/lowlevel_file.c
 *
 * Oracle: Algebraic — Invariant (4d)
 * Stronger considered:
 *   - State Machine (3): rejected — clear_file is a single-call mutation with no lifecycle or state field.
 *   - Differential (7): rejected — there is no independent reference implementation to compare against.
 *   - Round-trip (4a): rejected — zeroing is lossy, so there is no inverse.
 * Weaker available: Negative/Error Contract (4e), Crash-Only (6)
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_CLEAR_BYTES 1024U
#define MAX_EXTENT_PAGES 4U

struct clear_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char fill;
};

static unsigned choice_u32(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res clear_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct clear_case *cc = malloc(sizeof(*cc));
    if (cc == NULL) return THEFT_ALLOC_ERROR;

    cc->start_page = choice_u32(t, 8U);
    cc->page_count = 1U + choice_u32(t, MAX_EXTENT_PAGES);

    switch (theft_random_choice(t, 5)) {
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
    default:
        cc->page_off = choice_u32(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = cc->page_count * PG_SIZE - cc->page_off;
    if (span_bytes == 0U) span_bytes = 1U;
    unsigned max_len = span_bytes < MAX_CLEAR_BYTES ? span_bytes : MAX_CLEAR_BYTES;
    cc->len = 1U + choice_u32(t, max_len);
    cc->fill = (unsigned char)theft_random_choice(t, 256U);

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
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, fill=%u}",
            cc->start_page, cc->page_count, cc->page_off, cc->len, cc->fill);
}

static struct theft_type_info clear_case_info = {
    .alloc = clear_case_alloc_cb,
    .free = clear_case_free_cb,
    .hash = clear_case_hash_cb,
    .print = clear_case_print_cb,
};

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

static struct Extent *find_extent(struct inode *node, unsigned page)
{
    for (struct Extent *cur = node->extents; cur != NULL; cur = cur->next) {
        if (page >= cur->start_page && page < cur->start_page + cur->length) {
            return cur;
        }
    }
    return NULL;
}

static int page_is_allocated(struct inode *node, unsigned page)
{
    struct Extent *ext = find_extent(node, page);
    return ext != NULL && ext->data != NULL;
}

static enum theft_trial_res prop_clear_zeros_target_span(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(cc);
    unsigned extent_bytes = case_extent_bytes(cc);

    node.extents = make_extent(cc->start_page, cc->page_count, cc->fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    clear_file(&node, offset, cc->len);

    unsigned char *after = malloc(extent_bytes);
    if (after == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(after, 0xCC, extent_bytes);
    file_read(&node, cc->start_page * PG_SIZE, extent_bytes, (char *)after);

    unsigned start = cc->page_off;
    unsigned end = start + cc->len;
    int ok = 1;
    for (unsigned i = 0; i < extent_bytes; i++) {
        unsigned expected = (i >= start && i < end) ? 0U : cc->fill;
        if (after[i] != expected) {
            fprintf(stderr,
                    "clear span mismatch at byte=%u got=%u expected=%u (off=%u len=%u)\n",
                    i, after[i], expected, offset, cc->len);
            ok = 0;
            break;
        }
    }

    free(after);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_clear_allocates_sparse_range(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(cc);

    clear_file(&node, offset, cc->len);

    if (node.extents == NULL) {
        fprintf(stderr, "clear_file did not allocate extents for offset=%u len=%u\n", offset, cc->len);
        return THEFT_TRIAL_FAIL;
    }

    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, cc->len);
    for (unsigned page = start_page; page <= end_page; page++) {
        if (!page_is_allocated(&node, page)) {
            fprintf(stderr, "page %u not allocated after clear_file (off=%u len=%u)\n",
                    page, offset, cc->len);
            free_extents(&node);
            return THEFT_TRIAL_FAIL;
        }
    }

    unsigned char *out = malloc(cc->len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, cc->len);
    file_read(&node, offset, cc->len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < cc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "sparse clear returned non-zero at byte=%u value=%u\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_clear_zero_len_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(cc);
    unsigned extent_bytes = case_extent_bytes(cc);

    node.extents = make_extent(cc->start_page, cc->page_count, cc->fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    unsigned char *before = malloc(extent_bytes);
    unsigned char *after = malloc(extent_bytes);
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }

    file_read(&node, cc->start_page * PG_SIZE, extent_bytes, (char *)before);
    clear_file(&node, offset, 0U);
    file_read(&node, cc->start_page * PG_SIZE, extent_bytes, (char *)after);

    int ok = memcmp(before, after, extent_bytes) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length clear modified bytes (off=%u)\n", offset);
    }

    free(before);
    free(after);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
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
        printf("  [%s] %s\n",                                         \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);     \
        if (res != THEFT_RUN_PASS) failures++;                        \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("clear_file property-based tests:\n");
    RUN_PROP("zeros_target_span", prop_clear_zeros_target_span, 300);
    RUN_PROP("allocates_sparse_range", prop_clear_allocates_sparse_range, 300);
    RUN_PROP("zero_len_is_noop", prop_clear_zero_len_is_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
