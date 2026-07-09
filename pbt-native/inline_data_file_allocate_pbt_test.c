/*
 * Property-based tests for file_allocate() in
 * eval/inline_data/optimization/lowlevel_file.c
 *
 * Oracle: state/invariant contract for inline-data-backed page allocation.
 * Inline-only ranges must not allocate page-table entries. Valid non-empty
 * ranges crossing beyond INLINE_DATA_SIZE must allocate every intersecting
 * post-inline page and zero-fill newly allocated bytes. Repeating allocation
 * is idempotent and preserves existing bytes. Empty or out-of-range ranges are
 * no-ops.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define TEST_PAGE_WINDOW 8U
#define MAX_ALLOC_BYTES ((3U * PG_SIZE) + 31U)
#define INLINE_TEST_CASES 256U
#define MAX_FILE_LOGICAL_BYTES MAX_FILE_SIZE

struct alloc_case {
    unsigned offset;
    unsigned len;
    unsigned char seed;
};

struct inline_case {
    unsigned offset;
    unsigned len;
    unsigned seed_page;
    unsigned char seed;
};

struct invalid_case {
    unsigned offset;
    unsigned len;
};

struct counter cnt = { .size = 0 };

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned inclusive_choice(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static unsigned offset_near_inline_boundary(struct theft *t)
{
    switch (theft_random_choice(t, 8)) {
    case 0:
        return 0U;
    case 1:
        return INLINE_DATA_SIZE > 0U ? INLINE_DATA_SIZE - 1U : 0U;
    case 2:
        return INLINE_DATA_SIZE;
    case 3:
        return INLINE_DATA_SIZE + 1U;
    case 4:
        return INLINE_DATA_SIZE + PG_SIZE - 1U;
    case 5:
        return INLINE_DATA_SIZE + PG_SIZE;
    case 6:
        return INLINE_DATA_SIZE + (bounded_choice(t, TEST_PAGE_WINDOW) * PG_SIZE);
    default:
        return bounded_choice(t, INLINE_DATA_SIZE + (TEST_PAGE_WINDOW * PG_SIZE));
    }
}

static enum theft_alloc_res alloc_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct alloc_case *ac = malloc(sizeof(*ac));
    if (ac == NULL) return THEFT_ALLOC_ERROR;

    ac->offset = offset_near_inline_boundary(t);
    unsigned storage_limit = INLINE_DATA_SIZE + (TEST_PAGE_WINDOW * PG_SIZE);
    if (ac->offset >= storage_limit) ac->offset = storage_limit - 1U;
    unsigned available = storage_limit - ac->offset;
    unsigned max_len = available < MAX_ALLOC_BYTES ? available : MAX_ALLOC_BYTES;
    ac->len = 1U + bounded_choice(t, max_len);
    ac->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = ac;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res inline_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct inline_case *ic = malloc(sizeof(*ic));
    if (ic == NULL) return THEFT_ALLOC_ERROR;

    ic->offset = bounded_choice(t, INLINE_DATA_SIZE);
    unsigned available = INLINE_DATA_SIZE - ic->offset;
    ic->len = available == 0U ? 0U : inclusive_choice(t, available);
    ic->seed_page = bounded_choice(t, TEST_PAGE_WINDOW);
    ic->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = ic;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res invalid_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct invalid_case *ic = malloc(sizeof(*ic));
    if (ic == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 6)) {
    case 0:
        ic->offset = bounded_choice(t, INLINE_DATA_SIZE + (TEST_PAGE_WINDOW * PG_SIZE));
        ic->len = 0U;
        break;
    case 1:
        ic->offset = MAX_FILE_LOGICAL_BYTES;
        ic->len = 1U;
        break;
    case 2:
        ic->offset = MAX_FILE_LOGICAL_BYTES - 1U;
        ic->len = 2U;
        break;
    case 3:
        ic->offset = MAX_FILE_LOGICAL_BYTES - (PG_SIZE / 2U);
        ic->len = PG_SIZE;
        break;
    case 4:
        ic->offset = UINT32_MAX - bounded_choice(t, PG_SIZE);
        ic->len = 1U + bounded_choice(t, PG_SIZE);
        break;
    default:
        ic->offset = MAX_FILE_LOGICAL_BYTES + bounded_choice(t, INLINE_DATA_SIZE);
        ic->len = 1U + bounded_choice(t, PG_SIZE);
        break;
    }

    *instance = ic;
    return THEFT_ALLOC_OK;
}

static void case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash alloc_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct alloc_case));
}

static theft_hash inline_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct inline_case));
}

static theft_hash invalid_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct invalid_case));
}

static void alloc_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct alloc_case *ac = instance;
    fprintf(f, "{offset=%u, len=%u, seed=%u}", ac->offset, ac->len, ac->seed);
}

static void inline_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct inline_case *ic = instance;
    fprintf(f, "{offset=%u, len=%u, seed_page=%u, seed=%u}",
            ic->offset, ic->len, ic->seed_page, ic->seed);
}

static void invalid_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct invalid_case *ic = instance;
    fprintf(f, "{offset=%u, len=%u}", ic->offset, ic->len);
}

static struct theft_type_info alloc_case_info = {
    .alloc = alloc_case_alloc_cb,
    .free = case_free_cb,
    .hash = alloc_case_hash_cb,
    .print = alloc_case_print_cb,
};

static struct theft_type_info inline_case_info = {
    .alloc = inline_case_alloc_cb,
    .free = case_free_cb,
    .hash = inline_case_hash_cb,
    .print = inline_case_print_cb,
};

static struct theft_type_info invalid_case_info = {
    .alloc = invalid_case_alloc_cb,
    .free = case_free_cb,
    .hash = invalid_case_hash_cb,
    .print = invalid_case_print_cb,
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

static void fill_page(unsigned char *page, unsigned page_index, unsigned char seed)
{
    for (unsigned i = 0; i < PG_SIZE; i++) {
        page[i] = (unsigned char)(seed + page_index * 13U + i * 17U);
    }
}

static int seed_page(struct indextb *tb, unsigned page_index, unsigned char seed)
{
    if (page_index >= INDEXTB_NUM) return 0;
    tb->index[page_index] = malloc_page();
    if (tb->index[page_index] == NULL) return 0;
    fill_page(tb->index[page_index], page_index, seed);
    return 1;
}

static unsigned post_inline_page_for_offset(unsigned absolute)
{
    return (absolute - INLINE_DATA_SIZE) / PG_SIZE;
}

static unsigned end_byte(unsigned offset, unsigned len)
{
    return offset + len - 1U;
}

static int range_touches_regular_pages(unsigned offset, unsigned len)
{
    return len > 0U && end_byte(offset, len) >= INLINE_DATA_SIZE;
}

static unsigned expected_first_page(unsigned offset)
{
    return offset > INLINE_DATA_SIZE ? post_inline_page_for_offset(offset) : 0U;
}

static unsigned expected_last_page(unsigned offset, unsigned len)
{
    return post_inline_page_for_offset(end_byte(offset, len));
}

static unsigned char *byte_ptr(struct indextb *tb, unsigned absolute)
{
    unsigned page = post_inline_page_for_offset(absolute);
    unsigned page_off = (absolute - INLINE_DATA_SIZE) % PG_SIZE;
    if (page >= INDEXTB_NUM || tb->index[page] == NULL) return NULL;
    return tb->index[page] + page_off;
}

static int regular_range_is_zeroed(struct indextb *tb, unsigned offset, unsigned len)
{
    if (!range_touches_regular_pages(offset, len)) return 1;

    unsigned start = offset > INLINE_DATA_SIZE ? offset : INLINE_DATA_SIZE;
    unsigned end = end_byte(offset, len);
    for (unsigned absolute = start; absolute <= end; absolute++) {
        unsigned char *ptr = byte_ptr(tb, absolute);
        if (ptr == NULL || *ptr != 0U) {
            fprintf(stderr, "zero-fill mismatch absolute=%u got=%d\n",
                    absolute, ptr == NULL ? -1 : (int)*ptr);
            return 0;
        }
    }
    return 1;
}

static int write_pattern_to_regular_range(struct indextb *tb, unsigned offset,
                                           unsigned len, unsigned char seed)
{
    if (!range_touches_regular_pages(offset, len)) return 1;

    unsigned start = offset > INLINE_DATA_SIZE ? offset : INLINE_DATA_SIZE;
    unsigned end = end_byte(offset, len);
    for (unsigned absolute = start; absolute <= end; absolute++) {
        unsigned char *ptr = byte_ptr(tb, absolute);
        if (ptr == NULL) return 0;
        *ptr = (unsigned char)(seed + absolute * 31U);
    }
    return 1;
}

static int regular_range_matches_pattern(struct indextb *tb, unsigned offset,
                                         unsigned len, unsigned char seed)
{
    if (!range_touches_regular_pages(offset, len)) return 1;

    unsigned start = offset > INLINE_DATA_SIZE ? offset : INLINE_DATA_SIZE;
    unsigned end = end_byte(offset, len);
    for (unsigned absolute = start; absolute <= end; absolute++) {
        unsigned char *ptr = byte_ptr(tb, absolute);
        unsigned char expected = (unsigned char)(seed + absolute * 31U);
        if (ptr == NULL || *ptr != expected) {
            fprintf(stderr, "pattern mismatch absolute=%u got=%d expected=%u\n",
                    absolute, ptr == NULL ? -1 : (int)*ptr, expected);
            return 0;
        }
    }
    return 1;
}

static unsigned allocated_page_count(const struct indextb *tb)
{
    unsigned count = 0U;
    for (unsigned page = 0; page < INDEXTB_NUM; page++) {
        if (tb->index[page] != NULL) count++;
    }
    return count;
}

static int snapshot_pointers(unsigned char **snapshot, const struct indextb *tb)
{
    for (unsigned page = 0; page < INDEXTB_NUM; page++) snapshot[page] = tb->index[page];
    return 1;
}

static int pointers_match(const unsigned char **snapshot, const struct indextb *tb)
{
    for (unsigned page = 0; page < INDEXTB_NUM; page++) {
        if (snapshot[page] != tb->index[page]) {
            fprintf(stderr, "page pointer changed page=%u before=%p after=%p\n",
                    page, (void *)snapshot[page], (void *)tb->index[page]);
            return 0;
        }
    }
    return 1;
}

static int page_matches_seed(const struct indextb *tb, unsigned page_index, unsigned char seed)
{
    if (page_index >= INDEXTB_NUM || tb->index[page_index] == NULL) return 0;
    for (unsigned i = 0; i < PG_SIZE; i++) {
        unsigned char expected = (unsigned char)(seed + page_index * 13U + i * 17U);
        if (tb->index[page_index][i] != expected) {
            fprintf(stderr, "seed page changed page=%u off=%u got=%u expected=%u\n",
                    page_index, i, tb->index[page_index][i], expected);
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_valid_range_allocates_expected_pages_zeroed(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    file_allocate(&node, ac->offset, ac->len);

    int ok = 1;
    if (range_touches_regular_pages(ac->offset, ac->len)) {
        unsigned first = expected_first_page(ac->offset);
        unsigned last = expected_last_page(ac->offset, ac->len);
        for (unsigned page = first; page <= last; page++) {
            if (tb.index[page] == NULL) {
                fprintf(stderr, "missing allocated page=%u first=%u last=%u offset=%u len=%u\n",
                        page, first, last, ac->offset, ac->len);
                ok = 0;
                break;
            }
        }
    }
    if (ok) ok = regular_range_is_zeroed(&tb, ac->offset, ac->len);

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_reallocate_is_idempotent_and_preserves_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    file_allocate(&node, ac->offset, ac->len);
    if (!write_pattern_to_regular_range(&tb, ac->offset, ac->len, ac->seed)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_FAIL;
    }

    unsigned pages_before = allocated_page_count(&tb);
    file_allocate(&node, ac->offset, ac->len);
    unsigned pages_after = allocated_page_count(&tb);

    int ok = pages_before == pages_after &&
             regular_range_matches_pattern(&tb, ac->offset, ac->len, ac->seed);
    if (!ok) {
        fprintf(stderr, "reallocation changed state pages=%u->%u offset=%u len=%u\n",
                pages_before, pages_after, ac->offset, ac->len);
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_inline_only_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct inline_case *ic = arg1;
    struct inode node;
    struct indextb tb;
    unsigned char *before[INDEXTB_NUM];
    init_inode(&node, &tb);

    if (!seed_page(&tb, ic->seed_page, ic->seed)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    snapshot_pointers(before, &tb);

    file_allocate(&node, ic->offset, ic->len);

    int ok = pointers_match((const unsigned char **)before, &tb) &&
             page_matches_seed(&tb, ic->seed_page, ic->seed);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_or_out_of_range_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct invalid_case *ic = arg1;
    struct inode node;
    struct indextb tb;
    unsigned char *before[INDEXTB_NUM];
    init_inode(&node, &tb);
    snapshot_pointers(before, &tb);

    file_allocate(&node, ic->offset, ic->len);

    int ok = pointers_match((const unsigned char **)before, &tb);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP_WITH_TYPE(name_, prop_, info_, trials_)                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { info_ },                                     \
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

    printf("inline_data file_allocate property-based tests:\n");
    RUN_PROP_WITH_TYPE("valid_range_allocates_expected_pages_zeroed",
                       prop_valid_range_allocates_expected_pages_zeroed, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("reallocate_is_idempotent_and_preserves_bytes",
                       prop_reallocate_is_idempotent_and_preserves_bytes, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("inline_only_allocation_is_noop",
                       prop_inline_only_allocation_is_noop, &inline_case_info, INLINE_TEST_CASES);
    RUN_PROP_WITH_TYPE("empty_or_out_of_range_allocation_is_noop",
                       prop_empty_or_out_of_range_allocation_is_noop, &invalid_case_info, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
