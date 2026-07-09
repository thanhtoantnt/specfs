/*
 * Property-based tests for file_allocate() in
 * eval/rbtree/optimization/lowlevel_file.c
 *
 * Oracle: state/invariant contract for rbtree-backed extent allocation.
 * For every valid non-empty byte range, all pages intersecting
 * [offset, offset + len) must become allocated and newly allocated bytes
 * must be zero-filled. Re-allocating an allocated range must be idempotent
 * and preserve bytes. Allocations served from the rbtree preallocation pool
 * must preserve existing allocated bytes and materialize the requested pages.
 * Empty or out-of-range allocations are no-ops.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"
#include "mballoc.h"

#define MAX_ALLOC_BYTES (3U * PG_SIZE + 17U)
#define TEST_PAGE_WINDOW 128U
#define MAX_TRACKED_ALLOCS 4096U

struct alloc_case {
    unsigned start_page;
    unsigned page_off;
    unsigned len;
    unsigned char seed;
};

struct prealloc_case {
    unsigned start_page;
    unsigned first_pages;
    unsigned gap_pages;
    unsigned second_pages;
    unsigned char seed;
};

struct invalid_case {
    unsigned offset;
    unsigned len;
};

static unsigned char *tracked_allocs[MAX_TRACKED_ALLOCS];
static unsigned tracked_alloc_count;

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res alloc_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct alloc_case *ac = malloc(sizeof(*ac));
    if (ac == NULL) return THEFT_ALLOC_ERROR;

    ac->start_page = bounded_choice(t, TEST_PAGE_WINDOW);
    switch (theft_random_choice(t, 5)) {
    case 0:
        ac->page_off = 0U;
        break;
    case 1:
        ac->page_off = PG_SIZE - 1U;
        break;
    case 2:
        ac->page_off = PG_SIZE - 16U;
        break;
    default:
        ac->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned remaining = (TEST_PAGE_WINDOW - ac->start_page) * PG_SIZE - ac->page_off;
    if (remaining == 0U) remaining = 1U;
    unsigned max_len = remaining < MAX_ALLOC_BYTES ? remaining : MAX_ALLOC_BYTES;
    ac->len = 1U + bounded_choice(t, max_len);
    ac->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = ac;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res prealloc_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct prealloc_case *pc = malloc(sizeof(*pc));
    if (pc == NULL) return THEFT_ALLOC_ERROR;

    pc->start_page = bounded_choice(t, TEST_PAGE_WINDOW / 2U);
    pc->first_pages = 1U + bounded_choice(t, 8U);
    pc->gap_pages = bounded_choice(t, 8U);
    unsigned used_before_second = pc->first_pages + pc->gap_pages;
    unsigned remaining_prealloc = ALLOC_GRANULARITY > used_before_second
        ? ALLOC_GRANULARITY - used_before_second
        : 1U;
    if (remaining_prealloc > 8U) remaining_prealloc = 8U;
    pc->second_pages = 1U + bounded_choice(t, remaining_prealloc);
    pc->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = pc;
    return THEFT_ALLOC_OK;
}

static enum theft_alloc_res invalid_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct invalid_case *ic = malloc(sizeof(*ic));
    if (ic == NULL) return THEFT_ALLOC_ERROR;

    if (theft_random_choice(t, 2) == 0) {
        ic->offset = bounded_choice(t, MAX_FILE_SIZE);
        ic->len = 0U;
    } else {
        unsigned back = bounded_choice(t, PG_SIZE);
        ic->offset = MAX_FILE_SIZE - back;
        unsigned room = MAX_FILE_SIZE - ic->offset;
        ic->len = room + 1U + bounded_choice(t, PG_SIZE);
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

static theft_hash prealloc_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct prealloc_case));
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

static void prealloc_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct prealloc_case *pc = instance;
    fprintf(f, "{start_page=%u, first_pages=%u, gap_pages=%u, second_pages=%u, seed=%u}",
            pc->start_page, pc->first_pages, pc->gap_pages, pc->second_pages, pc->seed);
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

static struct theft_type_info prealloc_case_info = {
    .alloc = prealloc_case_alloc_cb,
    .free = case_free_cb,
    .hash = prealloc_case_hash_cb,
    .print = prealloc_case_print_cb,
};

static struct theft_type_info invalid_case_info = {
    .alloc = invalid_case_alloc_cb,
    .free = case_free_cb,
    .hash = invalid_case_hash_cb,
    .print = invalid_case_print_cb,
};

unsigned char *malloc_contigous_pages(unsigned num)
{
    if (num == 0U || tracked_alloc_count >= MAX_TRACKED_ALLOCS) return NULL;
    unsigned char *data = calloc(num, PG_SIZE);
    if (data != NULL) tracked_allocs[tracked_alloc_count++] = data;
    return data;
}

static void free_tracked_allocations(void)
{
    for (unsigned i = 0; i < tracked_alloc_count; i++) free(tracked_allocs[i]);
    memset(tracked_allocs, 0, sizeof(tracked_allocs));
    tracked_alloc_count = 0U;
}

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
}

static void free_extent_metadata(struct inode *node)
{
    Extent *cur = node->extents;
    while (cur != NULL) {
        Extent *next = cur->next;
        free(cur);
        cur = next;
    }
    node->extents = NULL;
}

static void free_prealloc_tree(struct rb_node *node)
{
    if (node == NULL) return;
    free_prealloc_tree(node->left);
    free_prealloc_tree(node->right);
    free(node->prealloc);
    free(node);
}

static void destroy_inode(struct inode *node)
{
    free_extent_metadata(node);
    free_prealloc_tree(node->prealloc_tree);
    node->prealloc_tree = NULL;
    free_tracked_allocations();
}

static unsigned case_offset(const struct alloc_case *ac)
{
    return ac->start_page * PG_SIZE + ac->page_off;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    return (offset + len - 1U) / PG_SIZE;
}

static Extent *find_extent_test(struct inode *node, unsigned page)
{
    for (Extent *cur = node->extents; cur != NULL; cur = cur->next) {
        if (page >= cur->start_page && page < cur->start_page + cur->length) {
            return cur;
        }
    }
    return NULL;
}

static int page_is_allocated(struct inode *node, unsigned page)
{
    Extent *ext = find_extent_test(node, page);
    return ext != NULL && ext->data != NULL;
}

static unsigned total_allocated_pages(struct inode *node)
{
    unsigned total = 0U;
    for (Extent *cur = node->extents; cur != NULL; cur = cur->next) total += cur->length;
    return total;
}

static unsigned extent_count(struct inode *node)
{
    unsigned total = 0U;
    for (Extent *cur = node->extents; cur != NULL; cur = cur->next) total++;
    return total;
}

static unsigned char *byte_ptr(struct inode *node, unsigned absolute)
{
    unsigned page = absolute / PG_SIZE;
    Extent *ext = find_extent_test(node, page);
    if (ext == NULL || ext->data == NULL) return NULL;
    unsigned ext_offset = (page - ext->start_page) * PG_SIZE + (absolute % PG_SIZE);
    return ext->data + ext_offset;
}

static int write_pattern_to_allocated_range(struct inode *node, unsigned offset,
                                            unsigned len, unsigned char seed)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned char *ptr = byte_ptr(node, offset + i);
        if (ptr == NULL) return 0;
        *ptr = (unsigned char)(seed + (unsigned char)(i * 31U));
    }
    return 1;
}

static int range_matches_pattern(struct inode *node, unsigned offset,
                                 unsigned len, unsigned char seed)
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

static enum theft_trial_res prop_valid_range_allocates_all_pages_zeroed(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(ac);
    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, ac->len);

    file_allocate(&node, offset, ac->len);

    int ok = 1;
    for (unsigned page = start_page; page <= end_page; page++) {
        if (!page_is_allocated(&node, page)) {
            fprintf(stderr, "page %u in [%u,%u] was not allocated\n",
                    page, start_page, end_page);
            ok = 0;
            break;
        }
    }
    if (ok) ok = range_is_zeroed(&node, offset, ac->len);

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_reallocate_is_idempotent_and_preserves_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(ac);

    file_allocate(&node, offset, ac->len);
    if (!write_pattern_to_allocated_range(&node, offset, ac->len, ac->seed)) {
        destroy_inode(&node);
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

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_prealloc_pool_materializes_later_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct prealloc_case *pc = arg1;
    struct inode node = make_inode();
    unsigned first_offset = pc->start_page * PG_SIZE;
    unsigned first_len = pc->first_pages * PG_SIZE;
    unsigned second_page = pc->start_page + pc->first_pages + pc->gap_pages;
    unsigned second_offset = second_page * PG_SIZE;
    unsigned second_len = pc->second_pages * PG_SIZE;
    int ok = 1;

    file_allocate(&node, first_offset, first_len);
    if (!write_pattern_to_allocated_range(&node, first_offset, first_len, pc->seed)) {
        destroy_inode(&node);
        return THEFT_TRIAL_FAIL;
    }

    file_allocate(&node, second_offset, second_len);

    for (unsigned page = pc->start_page; ok && page < pc->start_page + pc->first_pages; page++) {
        if (!page_is_allocated(&node, page)) ok = 0;
    }
    for (unsigned page = second_page; ok && page < second_page + pc->second_pages; page++) {
        if (!page_is_allocated(&node, page)) ok = 0;
    }
    if (ok) ok = range_matches_pattern(&node, first_offset, first_len, pc->seed);
    if (ok) ok = range_is_zeroed(&node, second_offset, second_len);

    if (!ok) {
        fprintf(stderr,
                "prealloc allocation failed: first_page=%u first_pages=%u gap=%u second_pages=%u\n",
                pc->start_page, pc->first_pages, pc->gap_pages, pc->second_pages);
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_or_out_of_range_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct invalid_case *ic = arg1;
    struct inode node = make_inode();

    file_allocate(&node, ic->offset, ic->len);

    int ok = node.extents == NULL && node.prealloc_tree == NULL;
    if (!ok) {
        fprintf(stderr, "invalid allocation mutated inode: offset=%u len=%u\n",
                ic->offset, ic->len);
    }

    destroy_inode(&node);
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

    printf("rbtree file_allocate property-based tests:\n");
    RUN_PROP_WITH_TYPE("valid_range_allocates_all_pages_zeroed",
                       prop_valid_range_allocates_all_pages_zeroed, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("reallocate_is_idempotent_and_preserves_bytes",
                       prop_reallocate_is_idempotent_and_preserves_bytes, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("prealloc_pool_materializes_later_pages",
                       prop_prealloc_pool_materializes_later_pages, &prealloc_case_info, 300);
    RUN_PROP_WITH_TYPE("empty_or_out_of_range_allocation_is_noop",
                       prop_empty_or_out_of_range_allocation_is_noop, &invalid_case_info, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
