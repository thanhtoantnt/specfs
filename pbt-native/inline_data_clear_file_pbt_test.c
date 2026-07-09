/*
 * Property-based tests for clear_file() in
 * eval/inline_data/optimization/lowlevel_file.c
 *
 * Oracle: Algebraic — Invariant (4d). clear_file(node, start, len)
 * must zero exactly [start, start + len), preserving all bytes outside the
 * target range. For sparse ranges it should materialize the touched post-inline
 * pages as zero-filled storage. len == 0 and ranges beyond MAX_FILE_SIZE are
 * no-ops.
 *
 * Stronger considered:
 *   - State Machine (3): rejected — clear_file is a single-call mutation over inode data.
 *   - Differential (7): rejected — no independent inline-data implementation is present.
 *   - Round-trip (4a): rejected — clearing is intentionally lossy.
 * Weaker available: Crash-Only (6).
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define TEST_PAGES 6U
#define TEST_STORAGE_BYTES (INLINE_DATA_SIZE + (TEST_PAGES * PG_SIZE))
#define MAX_CLEAR_BYTES ((2U * PG_SIZE) + 128U)

struct clear_case {
    unsigned offset;
    unsigned len;
    unsigned snapshot_len;
    unsigned oob_tail;
    unsigned oob_extra;
    unsigned char fill;
};

struct counter cnt = { .size = 0 };

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static unsigned random_below(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned random_inclusive(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static unsigned near_boundary(struct theft *t, unsigned center, unsigned radius)
{
    unsigned delta = random_inclusive(t, radius);
    if (theft_random_choice(t, 2) == 0) {
        return center > delta ? center - delta : 0U;
    }
    return TEST_STORAGE_BYTES - center <= delta ? TEST_STORAGE_BYTES - 1U : center + delta;
}

static enum theft_alloc_res clear_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct clear_case *cc = malloc(sizeof(*cc));
    if (cc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 11)) {
    case 0:
        cc->offset = 0U;
        break;
    case 1:
        cc->offset = INLINE_DATA_SIZE > 0U ? INLINE_DATA_SIZE - 1U : 0U;
        break;
    case 2:
        cc->offset = INLINE_DATA_SIZE;
        break;
    case 3:
        cc->offset = INLINE_DATA_SIZE + 1U;
        break;
    case 4:
        cc->offset = near_boundary(t, INLINE_DATA_SIZE, 256U);
        break;
    case 5:
        cc->offset = near_boundary(t, INLINE_DATA_SIZE + PG_SIZE, 256U);
        break;
    case 6:
        cc->offset = INLINE_DATA_SIZE + (random_below(t, TEST_PAGES) * PG_SIZE);
        break;
    case 7:
        cc->offset = INLINE_DATA_SIZE + (random_below(t, TEST_PAGES) * PG_SIZE) + (PG_SIZE - 1U);
        if (cc->offset >= TEST_STORAGE_BYTES) cc->offset = TEST_STORAGE_BYTES - 1U;
        break;
    default:
        cc->offset = random_below(t, TEST_STORAGE_BYTES);
        break;
    }

    unsigned available = TEST_STORAGE_BYTES - cc->offset;
    unsigned max_len = available < MAX_CLEAR_BYTES ? available : MAX_CLEAR_BYTES;
    cc->len = 1U + random_below(t, max_len);

    unsigned end = cc->offset + cc->len;
    unsigned trailing = TEST_STORAGE_BYTES - end;
    if (trailing > PG_SIZE) trailing = PG_SIZE;
    cc->snapshot_len = end + random_inclusive(t, trailing);
    cc->oob_tail = 1U + random_below(t, 64U);
    cc->oob_extra = 1U + random_below(t, 128U);
    cc->fill = (unsigned char)(1U + random_below(t, 255U));

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
    fprintf(f, "{offset=%u, len=%u, snapshot_len=%u, oob_tail=%u, oob_extra=%u, fill=%u}",
            cc->offset, cc->len, cc->snapshot_len, cc->oob_tail, cc->oob_extra, cc->fill);
}

static struct theft_type_info clear_case_info = {
    .alloc = clear_case_alloc_cb,
    .free = clear_case_free_cb,
    .hash = clear_case_hash_cb,
    .print = clear_case_print_cb,
};

static void init_inode(struct inode *node, struct indextb *tb)
{
    memset(node, 0, sizeof(*node));
    memset(tb, 0, sizeof(*tb));
    node->file = tb;
}

static void cleanup_table(struct indextb *tb)
{
    for (unsigned page = 0; page < INDEXTB_NUM; page++) {
        free(tb->index[page]);
        tb->index[page] = NULL;
    }
}

static int allocate_test_pages(struct indextb *tb)
{
    for (unsigned page = 0; page < TEST_PAGES; page++) {
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
    }
    return 1;
}

static unsigned char storage_byte(const struct inode *node, unsigned offset)
{
    if (offset < INLINE_DATA_SIZE) {
        return (unsigned char)node->inline_data[offset];
    }

    unsigned adjusted = offset - INLINE_DATA_SIZE;
    unsigned page = adjusted / PG_SIZE;
    unsigned page_offset = adjusted % PG_SIZE;
    if (page >= INDEXTB_NUM || node->file->index[page] == NULL) return 0U;
    return node->file->index[page][page_offset];
}

static void snapshot_storage(const struct inode *node, unsigned len, unsigned char *out)
{
    for (unsigned i = 0; i < len; i++) {
        out[i] = storage_byte(node, i);
    }
}

static void fill_existing_storage(struct inode *node, unsigned char *model, unsigned char fill)
{
    for (unsigned i = 0; i < TEST_STORAGE_BYTES; i++) {
        model[i] = (unsigned char)(fill + 1U + ((i * 37U) % 251U));
    }

    memcpy(node->inline_data, model, INLINE_DATA_SIZE);
    for (unsigned page = 0; page < TEST_PAGES; page++) {
        memcpy(node->file->index[page],
               model + INLINE_DATA_SIZE + page * PG_SIZE,
               PG_SIZE);
    }
}

static void apply_model_clear(unsigned char *model, const struct clear_case *cc)
{
    for (unsigned i = 0; i < cc->len; i++) {
        model[cc->offset + i] = 0U;
    }
}

static int compare_or_report(const unsigned char *actual, const unsigned char *expected,
                             unsigned len, const char *name,
                             const struct clear_case *cc)
{
    for (unsigned i = 0; i < len; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr,
                    "%s mismatch at absolute byte %u: got=%u expected=%u offset=%u len=%u snapshot=%u\n",
                    name, i, actual[i], expected[i], cc->offset, cc->len, cc->snapshot_len);
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_clear_zeros_target_span_and_preserves_rest(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);
    if (!allocate_test_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(TEST_STORAGE_BYTES);
    unsigned char *actual = malloc(cc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_existing_storage(&node, expected, cc->fill);
    apply_model_clear(expected, cc);
    clear_file(&node, cc->offset, cc->len);
    snapshot_storage(&node, cc->snapshot_len, actual);

    int ok = compare_or_report(actual, expected, cc->snapshot_len,
                               "clear_span", cc);

    free(expected);
    free(actual);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_clear_materializes_zero_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    clear_file(&node, cc->offset, cc->len);

    int ok = 1;
    for (unsigned i = 0; i < cc->len; i++) {
        unsigned absolute = cc->offset + i;
        if (storage_byte(&node, absolute) != 0U) {
            fprintf(stderr, "sparse clear byte is nonzero absolute=%u offset=%u len=%u\n",
                    absolute, cc->offset, cc->len);
            ok = 0;
            break;
        }
    }

    if (ok && cc->offset + cc->len > INLINE_DATA_SIZE) {
        unsigned first = cc->offset > INLINE_DATA_SIZE ?
                         (cc->offset - INLINE_DATA_SIZE) / PG_SIZE : 0U;
        unsigned last = (cc->offset + cc->len - 1U - INLINE_DATA_SIZE) / PG_SIZE;
        for (unsigned page = first; page <= last && page < TEST_PAGES; page++) {
            if (tb.index[page] == NULL) {
                fprintf(stderr, "sparse clear did not allocate page=%u offset=%u len=%u\n",
                        page, cc->offset, cc->len);
                ok = 0;
                break;
            }
        }
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_clear_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);
    if (!allocate_test_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *before = malloc(TEST_STORAGE_BYTES);
    unsigned char *after = malloc(TEST_STORAGE_BYTES);
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_existing_storage(&node, before, cc->fill);
    clear_file(&node, cc->offset, 0U);
    snapshot_storage(&node, TEST_STORAGE_BYTES, after);

    int ok = memcmp(before, after, TEST_STORAGE_BYTES) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length clear modified storage offset=%u\n", cc->offset);
    }

    free(before);
    free(after);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_out_of_range_clear_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct clear_case *cc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    unsigned final_page = INDEXTB_NUM - 1U;
    tb.index[final_page] = malloc_page();
    if (tb.index[final_page] == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memset(tb.index[final_page], cc->fill, PG_SIZE);

    unsigned char before[PG_SIZE];
    memcpy(before, tb.index[final_page], sizeof(before));

    unsigned offset = MAX_FILE_SIZE - cc->oob_tail;
    unsigned len = cc->oob_tail + cc->oob_extra;
    clear_file(&node, offset, len);

    int ok = memcmp(before, tb.index[final_page], sizeof(before)) == 0;
    if (!ok) {
        fprintf(stderr, "out-of-range clear modified final page offset=%u len=%u tail=%u extra=%u\n",
                offset, len, cc->oob_tail, cc->oob_extra);
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                  \
        struct theft_run_config cfg = {                                   \
            .name = name_,                                                \
            .prop1 = prop_,                                               \
            .type_info = { &clear_case_info },                            \
            .trials = trials_,                                            \
            .seed = theft_seed_of_time(),                                 \
        };                                                                \
        enum theft_run_res res = theft_run(&cfg);                         \
        printf("  [%s] %s\n",                                            \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);         \
        if (res != THEFT_RUN_PASS) failures++;                            \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("inline_data clear_file property-based tests:\n");
    RUN_PROP("clear_zeros_target_span_and_preserves_rest",
             prop_clear_zeros_target_span_and_preserves_rest, 300);
    RUN_PROP("sparse_clear_materializes_zero_bytes",
             prop_sparse_clear_materializes_zero_bytes, 300);
    RUN_PROP("zero_length_clear_is_noop",
             prop_zero_length_clear_is_noop, 150);
    RUN_PROP("out_of_range_clear_is_noop",
             prop_out_of_range_clear_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
