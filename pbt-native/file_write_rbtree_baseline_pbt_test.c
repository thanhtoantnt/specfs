/*
 * Property-based tests for file_write() in
 * eval/rbtree/baseline/lowlevel_file.c
 *
 * Oracle: Algebraic — round-trip plus invariant over extent-backed file_write.
 * Stronger considered:
 *   - State Machine: rejected — file_write has no explicit lifecycle state or
 *     state-dependent branching API.
 *   - Differential: rejected — no second independently meaningful
 *     implementation of the same contract.
 * Weaker available: Reference, Negative/Error Contract, Crash-Only
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_WRITE_BYTES 2048U
#define MAX_TEST_PAGES 4U
#define MAX_TEST_PAGE 16U

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_WRITE_BYTES];
    unsigned char fill;
};

struct out_of_range_case {
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

    memset(wc, 0, sizeof(*wc));

    wc->start_page = bounded_choice(t, MAX_TEST_PAGE);
    wc->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        wc->page_off = 0U;
        break;
    case 1:
        wc->page_off = 1U;
        break;
    case 2:
        wc->page_off = PG_SIZE - 1U;
        break;
    case 3:
        wc->page_off = PG_SIZE - 16U;
        break;
    case 4:
        wc->page_off = PG_SIZE / 2U;
        break;
    default:
        wc->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = wc->page_count * PG_SIZE - wc->page_off;
    unsigned max_len = span_bytes < MAX_WRITE_BYTES ? span_bytes : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256U);
    }
    wc->data[0] = (unsigned char)(1U + bounded_choice(t, 255U));
    wc->fill = (unsigned char)(1U + bounded_choice(t, 255U));

    *instance = wc;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res out_of_range_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct out_of_range_case *oc = malloc(sizeof(*oc));
    if (oc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 4U)) {
    case 0:
        oc->page_off = PG_SIZE - 1U;
        break;
    case 1:
        oc->page_off = PG_SIZE - 16U;
        break;
    case 2:
        oc->page_off = PG_SIZE - 128U;
        break;
    default:
        oc->page_off = PG_SIZE - 1U - bounded_choice(t, 128U);
        break;
    }

    unsigned available = PG_SIZE - oc->page_off;
    oc->len = available + 1U + bounded_choice(t, MAX_WRITE_BYTES - available - 1U);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        oc->data[i] = (unsigned char)theft_random_choice(t, 256U);
    }
    oc->data[0] = (unsigned char)(1U + bounded_choice(t, 255U));

    *instance = oc;
    return THEFT_ALLOC_OK;
}

static void write_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static void out_of_range_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash write_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct write_case));
}

static theft_hash out_of_range_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct out_of_range_case));
}

static void write_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct write_case *wc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, fill=%u}",
            wc->start_page, wc->page_count, wc->page_off, wc->len, wc->fill);
}

static void out_of_range_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct out_of_range_case *oc = instance;
    fprintf(f, "{page_off=%u, len=%u}", oc->page_off, oc->len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static struct theft_type_info out_of_range_case_info = {
    .alloc = out_of_range_case_alloc_cb,
    .free = out_of_range_case_free_cb,
    .hash = out_of_range_case_hash_cb,
    .print = out_of_range_case_print_cb,
};

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
}

static void free_extents(struct inode *node)
{
    Extent *cur = node->extents;
    while (cur != NULL) {
        Extent *next = cur->next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    node->extents = NULL;
}

static void free_preallocs(struct inode *node)
{
    struct Prealloc *cur = node->preallocs;
    while (cur != NULL) {
        struct Prealloc *next = cur->next;
        free(cur);
        cur = next;
    }
    node->preallocs = NULL;
}

static void destroy_inode(struct inode *node)
{
    free_extents(node);
    free_preallocs(node);
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static unsigned case_extent_bytes(const struct write_case *wc)
{
    return wc->page_count * PG_SIZE;
}

static int read_back(struct inode *node, unsigned offset, unsigned len, unsigned char *out)
{
    memset(out, 0xCC, len == 0U ? 1U : len);
    file_read(node, offset, len, (char *)out);
    return 1;
}

static struct Extent *make_extent(unsigned start_page, unsigned page_count, unsigned char fill)
{
    struct Extent *ext = malloc(sizeof(*ext));
    if (ext == NULL) return NULL;

    ext->data = malloc(page_count * PG_SIZE);
    if (ext->data == NULL) {
        free(ext);
        return NULL;
    }

    ext->start_page = start_page;
    ext->length = page_count;
    ext->next = NULL;
    memset(ext->data, fill, page_count * PG_SIZE);
    return ext;
}

static enum theft_trial_res prop_sparse_write_roundtrips(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);

    file_write(&node, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, offset, wc->len, out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) {
        fprintf(stderr, "sparse write roundtrip mismatch offset=%u len=%u\n", offset, wc->len);
    }

    free(out);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_allocated_write_preserves_unrelated_bytes(struct theft *t, void *arg1)
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
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, wc->start_page * PG_SIZE, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned i = 0; i < wc->page_off; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "prefix corrupted at %u got=%u expected=%u\n",
                    i, snapshot[i], fill);
            ok = 0;
            break;
        }
    }
    if (ok && memcmp(snapshot + wc->page_off, wc->data, wc->len) != 0) {
        fprintf(stderr, "written span mismatch offset=%u len=%u\n", offset, wc->len);
        ok = 0;
    }
    for (unsigned i = wc->page_off + wc->len; ok && i < extent_bytes; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "suffix corrupted at %u got=%u expected=%u\n",
                    i, snapshot[i], fill);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_data_zero_fills(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);

    node.extents = make_extent(wc->start_page, wc->page_count, 0x7B);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, offset, wc->len, NULL);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, offset, wc->len, out);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "zero-fill mismatch at %u got=%u\n", i, out[i]);
            ok = 0;
            break;
        }
    }

    free(out);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned extent_bytes = case_extent_bytes(wc);
    unsigned offset = case_offset(wc);
    unsigned char fill = 0x5C;

    node.extents = make_extent(wc->start_page, wc->page_count, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, offset, 0U, (const char *)wc->data);

    unsigned char *snapshot = malloc(extent_bytes);
    if (snapshot == NULL) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }
    read_back(&node, wc->start_page * PG_SIZE, extent_bytes, snapshot);

    int ok = 1;
    for (unsigned i = 0; i < extent_bytes; i++) {
        if (snapshot[i] != fill) {
            fprintf(stderr, "zero-length write changed byte %u\n", i);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_out_of_range_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct out_of_range_case *oc = arg1;
    struct inode node = make_inode();
    unsigned last_page = INDEXTB_NUM - 1U;
    unsigned offset = last_page * PG_SIZE + oc->page_off;
    unsigned char fill = 0xAB;

    node.extents = make_extent(last_page, 1U, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    unsigned char before[PG_SIZE];
    read_back(&node, last_page * PG_SIZE, PG_SIZE, before);

    file_write(&node, offset, oc->len, (const char *)oc->data);

    unsigned char after[PG_SIZE];
    read_back(&node, last_page * PG_SIZE, PG_SIZE, after);

    int ok = memcmp(before, after, PG_SIZE) == 0;
    if (!ok) {
        fprintf(stderr, "partial out-of-range write mutated last page offset=%u len=%u\n",
                offset, oc->len);
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_, type_info_)                    \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { type_info_ },                               \
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

    printf("rbtree baseline file_write property-based tests:\n");
    RUN_PROP("sparse_write_roundtrips", prop_sparse_write_roundtrips, 300, &write_case_info);
    RUN_PROP("allocated_write_preserves_unrelated_bytes", prop_allocated_write_preserves_unrelated_bytes, 300, &write_case_info);
    RUN_PROP("null_data_zero_fills", prop_null_data_zero_fills, 200, &write_case_info);
    RUN_PROP("zero_length_write_is_noop", prop_zero_length_write_is_noop, 200, &write_case_info);
    RUN_PROP("out_of_range_write_is_noop", prop_out_of_range_write_is_noop, 200, &out_of_range_case_info);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
