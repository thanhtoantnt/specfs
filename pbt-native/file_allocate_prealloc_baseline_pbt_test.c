/*
 * Property-based tests for file_allocate() in
 * eval/pre_alloc/baseline/lowlevel_file.c
 *
 * Oracle: state/invariant contract for extent-backed allocation. For every
 * valid non-empty byte range, every intersecting page must become allocated,
 * newly allocated bytes must be zero-filled, and no unrelated page should be
 * allocated. Repeating or expanding allocation must preserve existing bytes.
 * Empty and out-of-range allocations are no-ops.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_ALLOC_BYTES (3U * PG_SIZE + 17U)
#define TEST_PAGE_WINDOW 64U

struct alloc_case {
    unsigned start_page;
    unsigned page_off;
    unsigned len;
    unsigned char seed;
};

struct expand_case {
    unsigned first_page;
    unsigned first_pages;
    unsigned before_pages;
    unsigned after_pages;
    unsigned char seed;
};

struct invalid_case {
    unsigned offset;
    unsigned len;
    unsigned char seed;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned draw_page_offset(struct theft *t)
{
    switch (theft_random_choice(t, 5U)) {
    case 0:
        return 0U;
    case 1:
        return PG_SIZE - 1U;
    case 2:
        return PG_SIZE - 16U;
    case 3:
        return PG_SIZE / 2U;
    default:
        return bounded_choice(t, PG_SIZE);
    }
}

static enum theft_alloc_res alloc_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct alloc_case *ac = malloc(sizeof(*ac));
    if (ac == NULL) return THEFT_ALLOC_ERROR;

    ac->start_page = bounded_choice(t, TEST_PAGE_WINDOW);
    ac->page_off = draw_page_offset(t);
    unsigned remaining = (TEST_PAGE_WINDOW - ac->start_page) * PG_SIZE - ac->page_off;
    if (remaining == 0U) remaining = 1U;
    unsigned max_len = remaining < MAX_ALLOC_BYTES ? remaining : MAX_ALLOC_BYTES;
    ac->len = 1U + bounded_choice(t, max_len);
    ac->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = ac;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res expand_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct expand_case *ec = malloc(sizeof(*ec));
    if (ec == NULL) return THEFT_ALLOC_ERROR;

    ec->first_page = 2U + bounded_choice(t, TEST_PAGE_WINDOW - 8U);
    ec->first_pages = 1U + bounded_choice(t, 4U);
    ec->before_pages = bounded_choice(t, 3U);
    ec->after_pages = 1U + bounded_choice(t, 3U);
    ec->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = ec;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res invalid_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct invalid_case *ic = malloc(sizeof(*ic));
    if (ic == NULL) return THEFT_ALLOC_ERROR;

    if (theft_random_choice(t, 2U) == 0U) {
        ic->offset = bounded_choice(t, MAX_FILE_SIZE);
        ic->len = 0U;
    } else {
        unsigned back = bounded_choice(t, PG_SIZE);
        ic->offset = MAX_FILE_SIZE - back;
        unsigned room = MAX_FILE_SIZE - ic->offset;
        ic->len = room + 1U + bounded_choice(t, PG_SIZE);
    }
    ic->seed = (unsigned char)theft_random_choice(t, 256U);

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

static theft_hash expand_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct expand_case));
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
    fprintf(f, "{start_page=%u, page_off=%u, len=%u, seed=%u}",
            ac->start_page, ac->page_off, ac->len, ac->seed);
}

static void expand_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct expand_case *ec = instance;
    fprintf(f, "{first_page=%u, first_pages=%u, before_pages=%u, after_pages=%u, seed=%u}",
            ec->first_page, ec->first_pages, ec->before_pages, ec->after_pages, ec->seed);
}

static void invalid_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct invalid_case *ic = instance;
    fprintf(f, "{offset=%u, len=%u, seed=%u}", ic->offset, ic->len, ic->seed);
}

static struct theft_type_info alloc_case_info = {
    .alloc = alloc_case_alloc_cb,
    .free = case_free_cb,
    .hash = alloc_case_hash_cb,
    .print = alloc_case_print_cb,
};

static struct theft_type_info expand_case_info = {
    .alloc = expand_case_alloc_cb,
    .free = case_free_cb,
    .hash = expand_case_hash_cb,
    .print = expand_case_print_cb,
};

static struct theft_type_info invalid_case_info = {
    .alloc = invalid_case_alloc_cb,
    .free = case_free_cb,
    .hash = invalid_case_hash_cb,
    .print = invalid_case_print_cb,
};

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
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

static struct Extent *make_extent(unsigned start_page, unsigned page_count, unsigned char seed)
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
    for (unsigned i = 0; i < page_count * PG_SIZE; i++) {
        ext->data[i] = (unsigned char)(seed + (unsigned char)(i * 17U));
    }
    return ext;
}

static unsigned case_offset(const struct alloc_case *ac)
{
    return ac->start_page * PG_SIZE + ac->page_off;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    return (offset + len - 1U) / PG_SIZE;
}

static struct Extent *find_extent_test(struct inode *node, unsigned page)
{
    for (struct Extent *cur = node->extents; cur != NULL; cur = cur->next) {
        if (page >= cur->start_page && page < cur->start_page + cur->length) return cur;
    }
    return NULL;
}

static unsigned char *byte_ptr(struct inode *node, unsigned absolute)
{
    unsigned page = absolute / PG_SIZE;
    struct Extent *ext = find_extent_test(node, page);
    if (ext == NULL || ext->data == NULL) return NULL;
    unsigned ext_offset = (page - ext->start_page) * PG_SIZE + (absolute % PG_SIZE);
    return ext->data + ext_offset;
}

static unsigned total_allocated_pages(struct inode *node)
{
    unsigned total = 0U;
    for (struct Extent *cur = node->extents; cur != NULL; cur = cur->next) total += cur->length;
    return total;
}

static unsigned extent_count(struct inode *node)
{
    unsigned total = 0U;
    for (struct Extent *cur = node->extents; cur != NULL; cur = cur->next) total++;
    return total;
}

static int page_is_allocated(struct inode *node, unsigned page)
{
    struct Extent *ext = find_extent_test(node, page);
    return ext != NULL && ext->data != NULL;
}

static int range_is_zeroed(struct inode *node, unsigned offset, unsigned len)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned char *ptr = byte_ptr(node, offset + i);
        if (ptr == NULL || *ptr != 0U) {
            fprintf(stderr, "zero-fill mismatch at byte=%u got=%d\n",
                    i, ptr == NULL ? -1 : (int)*ptr);
            return 0;
        }
    }
    return 1;
}

static int write_pattern(struct inode *node, unsigned offset, unsigned len, unsigned char seed)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned char *ptr = byte_ptr(node, offset + i);
        if (ptr == NULL) return 0;
        *ptr = (unsigned char)(seed + (unsigned char)(i * 31U));
    }
    return 1;
}

static int range_matches_pattern(struct inode *node, unsigned offset, unsigned len, unsigned char seed)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned char *ptr = byte_ptr(node, offset + i);
        unsigned char expected = (unsigned char)(seed + (unsigned char)(i * 31U));
        if (ptr == NULL || *ptr != expected) {
            fprintf(stderr, "pattern mismatch at byte=%u got=%d expected=%u\n",
                    i, ptr == NULL ? -1 : (int)*ptr, expected);
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_valid_range_allocates_all_pages_zeroed(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(ac);
    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, ac->len);
    unsigned expected_pages = end_page - start_page + 1U;

    file_allocate(&node, offset, ac->len);

    int ok = total_allocated_pages(&node) == expected_pages;
    if (!ok) {
        fprintf(stderr, "unexpected allocated page count got=%u expected=%u\n",
                total_allocated_pages(&node), expected_pages);
    }
    for (unsigned page = start_page; ok && page <= end_page; page++) {
        if (!page_is_allocated(&node, page)) {
            fprintf(stderr, "page %u in [%u,%u] was not allocated\n",
                    page, start_page, end_page);
            ok = 0;
        }
    }
    if (ok) ok = range_is_zeroed(&node, offset, ac->len);

    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_reallocate_is_idempotent_and_preserves_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(ac);

    file_allocate(&node, offset, ac->len);
    if (!write_pattern(&node, offset, ac->len, ac->seed)) {
        free_extents(&node);
        return THEFT_TRIAL_FAIL;
    }

    unsigned pages_before = total_allocated_pages(&node);
    unsigned extents_before = extent_count(&node);
    file_allocate(&node, offset, ac->len);
    unsigned pages_after = total_allocated_pages(&node);
    unsigned extents_after = extent_count(&node);

    int ok = pages_before == pages_after && extents_before == extents_after &&
             range_matches_pattern(&node, offset, ac->len, ac->seed);
    if (!ok) {
        fprintf(stderr,
                "reallocate changed state: pages %u->%u extents %u->%u offset=%u len=%u\n",
                pages_before, pages_after, extents_before, extents_after, offset, ac->len);
    }

    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_expanding_allocation_preserves_old_bytes_and_zeroes_new(struct theft *t, void *arg1)
{
    (void)t;
    const struct expand_case *ec = arg1;
    struct inode node = make_inode();
    unsigned first_offset = ec->first_page * PG_SIZE;
    unsigned first_len = ec->first_pages * PG_SIZE;
    unsigned expanded_page = ec->first_page - ec->before_pages;
    unsigned expanded_pages = ec->before_pages + ec->first_pages + ec->after_pages;
    unsigned expanded_offset = expanded_page * PG_SIZE;
    unsigned expanded_len = expanded_pages * PG_SIZE;

    file_allocate(&node, first_offset, first_len);
    if (!write_pattern(&node, first_offset, first_len, ec->seed)) {
        free_extents(&node);
        return THEFT_TRIAL_FAIL;
    }

    file_allocate(&node, expanded_offset, expanded_len);

    int ok = 1;
    for (unsigned page = expanded_page; ok && page < expanded_page + expanded_pages; page++) {
        if (!page_is_allocated(&node, page)) {
            fprintf(stderr, "expanded allocation missed page=%u\n", page);
            ok = 0;
        }
    }
    if (ok) ok = range_matches_pattern(&node, first_offset, first_len, ec->seed);
    if (ok && ec->before_pages > 0U) {
        ok = range_is_zeroed(&node, expanded_offset, ec->before_pages * PG_SIZE);
    }
    if (ok) {
        unsigned after_offset = (ec->first_page + ec->first_pages) * PG_SIZE;
        ok = range_is_zeroed(&node, after_offset, ec->after_pages * PG_SIZE);
    }

    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_or_out_of_range_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct invalid_case *ic = arg1;
    struct inode node = make_inode();
    node.extents = make_extent(0U, 1U, ic->seed);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    unsigned char before[PG_SIZE];
    memcpy(before, node.extents->data, sizeof(before));
    struct Extent *extent_before = node.extents;
    unsigned char *data_before = node.extents->data;

    file_allocate(&node, ic->offset, ic->len);

    int ok = node.extents == extent_before && node.extents->data == data_before &&
             node.extents->length == 1U && memcmp(before, node.extents->data, sizeof(before)) == 0;
    if (!ok) {
        fprintf(stderr, "invalid allocation mutated inode: offset=%u len=%u\n",
                ic->offset, ic->len);
    }

    free_extents(&node);
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

    printf("pre_alloc baseline file_allocate property-based tests:\n");
    RUN_PROP_WITH_TYPE("valid_range_allocates_all_pages_zeroed",
                       prop_valid_range_allocates_all_pages_zeroed, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("reallocate_is_idempotent_and_preserves_bytes",
                       prop_reallocate_is_idempotent_and_preserves_bytes, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("expanding_allocation_preserves_old_bytes_and_zeroes_new",
                       prop_expanding_allocation_preserves_old_bytes_and_zeroes_new, &expand_case_info, 300);
    RUN_PROP_WITH_TYPE("empty_or_out_of_range_allocation_is_noop",
                       prop_empty_or_out_of_range_allocation_is_noop, &invalid_case_info, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
