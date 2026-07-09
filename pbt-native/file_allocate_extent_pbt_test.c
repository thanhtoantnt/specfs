/*
 * Property-based tests for file_allocate() in
 * eval/extent/optimization/lowlevel_file.c
 *
 * Oracle: state/invariant contract for extent allocation. For every valid
 * non-empty byte range, all pages intersecting [offset, offset + len) must be
 * allocated and newly allocated bytes must read back as zeroes. Re-allocation
 * of an already allocated range must be idempotent and preserve existing bytes.
 * Invalid empty/out-of-range allocations must be no-ops. A null inode must be
 * handled safely rather than crashing.
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

static enum theft_alloc_res invalid_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct invalid_case *ic = malloc(sizeof(*ic));
    if (ic == NULL) return THEFT_ALLOC_ERROR;

    if (theft_random_choice(t, 2) == 0) {
        /* Empty allocation is explicitly a no-op at arbitrary in-range offset. */
        ic->offset = bounded_choice(t, MAX_FILE_SIZE);
        ic->len = 0U;
    } else {
        /* Non-empty range that crosses MAX_FILE_SIZE without unsigned overflow. */
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
    struct Extent *cur = node->extents;
    while (cur != NULL) {
        struct Extent *next = cur->next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    node->extents = NULL;
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
        if (page >= cur->start_page && page < cur->start_page + cur->length) {
            return cur;
        }
    }
    return NULL;
}

static int page_is_allocated(struct inode *node, unsigned page)
{
    struct Extent *ext = find_extent_test(node, page);
    return ext != NULL && ext->data != NULL;
}

static unsigned total_allocated_pages(struct inode *node)
{
    unsigned total = 0U;
    for (struct Extent *cur = node->extents; cur != NULL; cur = cur->next) {
        total += cur->length;
    }
    return total;
}

static unsigned extent_count(struct inode *node)
{
    unsigned total = 0U;
    for (struct Extent *cur = node->extents; cur != NULL; cur = cur->next) {
        total++;
    }
    return total;
}

static int write_pattern_to_allocated_range(struct inode *node, unsigned offset,
                                            unsigned len, unsigned char seed)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned absolute = offset + i;
        unsigned page = absolute / PG_SIZE;
        unsigned page_off = absolute % PG_SIZE;
        struct Extent *ext = find_extent_test(node, page);
        if (ext == NULL || ext->data == NULL) return 0;
        unsigned ext_off = (page - ext->start_page) * PG_SIZE + page_off;
        ext->data[ext_off] = (unsigned char)(seed + (unsigned char)(i * 31U));
    }
    return 1;
}

static int buffer_matches_pattern(const unsigned char *buf, unsigned len, unsigned char seed)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned char expected = (unsigned char)(seed + (unsigned char)(i * 31U));
        if (buf[i] != expected) {
            fprintf(stderr, "pattern mismatch at %u: got=%u expected=%u\n",
                    i, buf[i], expected);
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

    unsigned char *out = malloc(ac->len);
    if (ok && out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    if (ok) {
        memset(out, 0xCC, ac->len);
        file_read(&node, offset, ac->len, (char *)out);
        for (unsigned i = 0; i < ac->len; i++) {
            if (out[i] != 0U) {
                fprintf(stderr, "new allocation not zeroed at byte %u: got=%u\n",
                        i, out[i]);
                ok = 0;
                break;
            }
        }
    }

    free(out);
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
    if (!write_pattern_to_allocated_range(&node, offset, ac->len, ac->seed)) {
        free_extents(&node);
        return THEFT_TRIAL_FAIL;
    }

    unsigned pages_before = total_allocated_pages(&node);
    unsigned extents_before = extent_count(&node);
    file_allocate(&node, offset, ac->len);
    unsigned pages_after = total_allocated_pages(&node);
    unsigned extents_after = extent_count(&node);

    unsigned char *out = malloc(ac->len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, ac->len);
    file_read(&node, offset, ac->len, (char *)out);

    int ok = pages_before == pages_after && extents_before == extents_after &&
             buffer_matches_pattern(out, ac->len, ac->seed);
    if (!ok) {
        fprintf(stderr,
                "reallocate changed state: pages %u->%u extents %u->%u offset=%u len=%u\n",
                pages_before, pages_after, extents_before, extents_after, offset, ac->len);
    }

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_or_out_of_range_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct invalid_case *ic = arg1;
    struct inode node = make_inode();

    file_allocate(&node, ic->offset, ic->len);

    int ok = node.extents == NULL;
    if (!ok) {
        fprintf(stderr, "invalid allocation mutated inode: offset=%u len=%u\n",
                ic->offset, ic->len);
    }

    free_extents(&node);
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

    printf("file_allocate property-based tests:\n");
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
