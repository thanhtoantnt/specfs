/*
 * Property-based tests for file_allocate() in
 * eval/rbtree/baseline/lowlevel_file.c
 *
 * Oracle: negative/error contract + algebraic invariants for range allocation.
 * Stronger considered:
 *   - State machine: rejected — file_allocate is a single-shot mutator with no
 *     exposed lifecycle or branching state.
 *   - Differential: rejected — there is no independent baseline implementation
 *     of this exact rbtree variant in the harness.
 * Weaker available: Reference, Crash-only.
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
#include "mballoc.h"

#define MAX_ALLOC_BYTES (3U * PG_SIZE + 17U)
#define TEST_PAGE_WINDOW 64U

struct alloc_case {
    unsigned start_page;
    unsigned page_off;
    unsigned len;
    unsigned char seed;
};

struct invalid_case {
    unsigned offset;
    unsigned len;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned draw_start_page(struct theft *t)
{
    switch (theft_random_choice(t, 6U)) {
    case 0:
        return 0U;
    case 1:
        return INDEXTB_NUM - 1U;
    case 2:
        return INDEXTB_NUM - 2U;
    default:
        return bounded_choice(t, TEST_PAGE_WINDOW);
    }
}

static unsigned draw_page_offset(struct theft *t)
{
    switch (theft_random_choice(t, 6U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return PG_SIZE - 1U;
    case 3:
        return PG_SIZE - 16U;
    case 4:
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

    ac->start_page = draw_start_page(t);
    ac->page_off = draw_page_offset(t);

    uint64_t offset = (uint64_t)ac->start_page * PG_SIZE + ac->page_off;
    uint64_t remaining = (uint64_t)MAX_FILE_SIZE - offset;
    unsigned max_len = remaining < MAX_ALLOC_BYTES ? (unsigned)remaining : MAX_ALLOC_BYTES;
    if (max_len == 0U) max_len = 1U;
    ac->len = 1U + bounded_choice(t, max_len);
    ac->seed = (unsigned char)theft_random_choice(t, 256U);

    *instance = ac;
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

static unsigned extent_page_count(struct inode *node)
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

static unsigned case_offset(const struct alloc_case *ac)
{
    return ac->start_page * PG_SIZE + ac->page_off;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    uint64_t sum = (uint64_t)offset + (uint64_t)len - 1U;
    return (unsigned)(sum / PG_SIZE);
}

static unsigned pages_in_range(unsigned offset, unsigned len)
{
    if (len == 0U) return 0U;
    return end_page_for_range(offset, len) - (offset / PG_SIZE) + 1U;
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
    unsigned expected_pages = pages_in_range(offset, ac->len);

    file_allocate(&node, offset, ac->len);

    int ok = extent_page_count(&node) == expected_pages;
    for (unsigned page = start_page; ok && page <= end_page; page++) {
        if (!page_is_allocated(&node, page)) {
            fprintf(stderr, "page %u in [%u,%u] was not allocated\n",
                    page, start_page, end_page);
            ok = 0;
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

    unsigned pages_before = extent_page_count(&node);
    unsigned extents_before = extent_count(&node);
    file_allocate(&node, offset, ac->len);
    unsigned pages_after = extent_page_count(&node);
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

static enum theft_trial_res prop_empty_or_out_of_range_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct invalid_case *ic = arg1;
    struct inode node = make_inode();

    file_allocate(&node, ic->offset, ic->len);

    int ok = node.extents == NULL && node.preallocs == NULL;
    if (!ok) {
        fprintf(stderr, "invalid allocation mutated inode: offset=%u len=%u\n",
                ic->offset, ic->len);
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_inode_is_safe(struct theft *t, void *arg1)
{
    (void)t;
    const struct alloc_case *ac = arg1;
    unsigned offset = case_offset(ac);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return THEFT_TRIAL_ERROR;
    }

    if (pid == 0) {
        file_allocate(NULL, offset, ac->len);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return THEFT_TRIAL_ERROR;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "null inode crashed or returned non-zero: status=%d offset=%u len=%u\n",
                status, offset, ac->len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
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

    printf("rbtree baseline file_allocate property-based tests:\n");
    RUN_PROP_WITH_TYPE("valid_range_allocates_all_pages_zeroed",
                       prop_valid_range_allocates_all_pages_zeroed, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("reallocate_is_idempotent_and_preserves_bytes",
                       prop_reallocate_is_idempotent_and_preserves_bytes, &alloc_case_info, 300);
    RUN_PROP_WITH_TYPE("empty_or_out_of_range_allocation_is_noop",
                       prop_empty_or_out_of_range_allocation_is_noop, &invalid_case_info, 200);
    RUN_PROP_WITH_TYPE("null_inode_is_safe",
                       prop_null_inode_is_safe, &alloc_case_info, 1);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
