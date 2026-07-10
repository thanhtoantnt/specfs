/*
 * Theft property-based tests for malloc_entry() in
 * eval/extent/baseline/util.c
 *
 * Oracle: Algebraic — allocator invariant / constructor storage contract.
 * Stronger considered:
 *   - State Machine: rejected — malloc_entry is a single allocation helper with no lifecycle state.
 *   - Differential: rejected — no independent trusted entry allocator implementation exists.
 *   - Round-trip: rejected — no inverse operation with observable structure beyond free().
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "util.h"

#define MAX_ENTRY_NAME_LEN 32U
#define CANARY_SIZE 32U
#define CANARY_BYTE 0xa5U

static void *(*real_malloc_fn)(size_t) = malloc;
static void (*real_free_fn)(void *) = free;
static void *(*real_memset_fn)(void *, int, size_t) = memset;

struct entry_case {
    char name[MAX_ENTRY_NAME_LEN + 1U];
    int use_name;
    int use_inode;
    int use_next;
};

struct tracked_alloc {
    unsigned char *raw;
    void *user;
    size_t requested;
};

static struct tracked_alloc last_alloc;
static unsigned malloc_calls;
static int tracker_enabled;

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static void fill_name(struct theft *t, char out[MAX_ENTRY_NAME_LEN + 1U])
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    unsigned len;

    switch (theft_random_choice(t, 8U)) {
    case 0:
        len = 0U;
        break;
    case 1:
        len = 1U;
        break;
    case 2:
        len = MAX_ENTRY_NAME_LEN;
        break;
    default:
        len = bounded_choice(t, MAX_ENTRY_NAME_LEN + 1U);
        break;
    }

    for (unsigned i = 0U; i < len; i++) {
        out[i] = alphabet[bounded_choice(t, (unsigned)(sizeof(alphabet) - 1U))];
    }
    out[len] = '\0';
}

static enum theft_alloc_res entry_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct entry_case *ec = calloc(1U, sizeof(*ec));
    if (ec == NULL) return THEFT_ALLOC_ERROR;

    fill_name(t, ec->name);
    ec->use_name = (int)theft_random_choice(t, 2U);
    ec->use_inode = (int)theft_random_choice(t, 2U);
    ec->use_next = (int)theft_random_choice(t, 2U);

    *instance = ec;
    return THEFT_ALLOC_OK;
}

static void entry_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash entry_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct entry_case));
}

static void entry_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct entry_case *ec = instance;
    fprintf(f, "{name=\"%s\", use_name=%d, use_inode=%d, use_next=%d}",
            ec->name, ec->use_name, ec->use_inode, ec->use_next);
}

static struct theft_type_info entry_case_info = {
    .alloc = entry_case_alloc_cb,
    .free = entry_case_free_cb,
    .hash = entry_case_hash_cb,
    .print = entry_case_print_cb,
};

static void reset_tracker(void)
{
    memset(&last_alloc, 0, sizeof(last_alloc));
    malloc_calls = 0U;
}

static void *tracked_malloc(size_t size)
{
    if (!tracker_enabled) {
        return real_malloc_fn(size);
    }

    malloc_calls++;
    size_t total = CANARY_SIZE + size + CANARY_SIZE;
    unsigned char *raw = real_malloc_fn(total == 0U ? 1U : total);
    if (raw == NULL) {
        return NULL;
    }

    real_memset_fn(raw, CANARY_BYTE, total == 0U ? 1U : total);
    last_alloc.raw = raw;
    last_alloc.user = raw + CANARY_SIZE;
    last_alloc.requested = size;
    return last_alloc.user;
}

static void tracked_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    if (ptr == last_alloc.user && last_alloc.raw != NULL) {
        real_free_fn(last_alloc.raw);
        memset(&last_alloc, 0, sizeof(last_alloc));
        return;
    }

    real_free_fn(ptr);
}

#define malloc tracked_malloc
#include "../eval/extent/baseline/util.c"
#undef malloc

static int canaries_intact(void)
{
    if (last_alloc.raw == NULL) {
        return 0;
    }

    for (size_t i = 0U; i < CANARY_SIZE; i++) {
        if (last_alloc.raw[i] != CANARY_BYTE) {
            return 0;
        }
    }
    for (size_t i = 0U; i < CANARY_SIZE; i++) {
        if (last_alloc.raw[CANARY_SIZE + last_alloc.requested + i] != CANARY_BYTE) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_allocates_exact_entry_size(struct theft *t, void *arg1)
{
    (void)t;
    (void)arg1;

    reset_tracker();
    tracker_enabled = 1;
    struct entry *entry = malloc_entry();
    tracker_enabled = 0;

    int ok = entry != NULL &&
             entry == (struct entry *)last_alloc.user &&
             malloc_calls == 1U &&
             last_alloc.requested == sizeof(struct entry) &&
             canaries_intact();

    tracked_free(entry);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_returned_entry_storage_is_writable(struct theft *t, void *arg1)
{
    (void)t;
    const struct entry_case *ec = arg1;
    struct inode inode_value;
    struct entry next_value;
    memset(&inode_value, 0x11, sizeof(inode_value));
    memset(&next_value, 0x22, sizeof(next_value));

    char *name_value = ec->use_name ? (char *)ec->name : NULL;
    void *inum_value = ec->use_inode ? (void *)&inode_value : NULL;
    struct entry *next_ptr = ec->use_next ? &next_value : NULL;

    reset_tracker();
    tracker_enabled = 1;
    struct entry *entry = malloc_entry();
    tracker_enabled = 0;
    if (entry == NULL) return THEFT_TRIAL_ERROR;

    memset(entry, 0x5a, sizeof(*entry));
    entry->name = name_value;
    entry->inum = inum_value;
    entry->next = next_ptr;

    int ok = entry->name == name_value &&
             entry->inum == inum_value &&
             entry->next == next_ptr &&
             canaries_intact();

    tracked_free(entry);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_each_call_returns_independent_storage(struct theft *t, void *arg1)
{
    (void)t;
    const struct entry_case *ec = arg1;
    struct inode first_inode;
    struct inode second_inode;
    struct entry first_next;
    struct entry second_next;
    char second_name[MAX_ENTRY_NAME_LEN + 8U];

    memset(&first_inode, 0x31, sizeof(first_inode));
    memset(&second_inode, 0x32, sizeof(second_inode));
    memset(&first_next, 0x41, sizeof(first_next));
    memset(&second_next, 0x42, sizeof(second_next));
    snprintf(second_name, sizeof(second_name), "other_%s", ec->name);

    struct entry *first = malloc_entry();
    struct entry *second = malloc_entry();
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return THEFT_TRIAL_ERROR;
    }

    second->name = second_name;
    second->inum = &second_inode;
    second->next = &second_next;

    first->name = ec->use_name ? (char *)ec->name : NULL;
    first->inum = ec->use_inode ? (void *)&first_inode : NULL;
    first->next = ec->use_next ? &first_next : NULL;

    int ok = first != second &&
             second->name == second_name &&
             second->inum == &second_inode &&
             second->next == &second_next;

    free(first);
    free(second);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                 \
        struct theft_run_config cfg = {                                  \
            .name = name_,                                               \
            .prop1 = prop_,                                              \
            .type_info = { &entry_case_info },                           \
            .trials = trials_,                                           \
            .seed = theft_seed_of_time(),                                \
        };                                                               \
        enum theft_run_res res = theft_run(&cfg);                        \
        printf("  [%s] %s\n",                                           \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                           \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("malloc_entry baseline property-based tests:\n");
    RUN_PROP("allocates_exact_entry_size", prop_allocates_exact_entry_size, 300);
    RUN_PROP("returned_entry_storage_is_writable", prop_returned_entry_storage_is_writable, 300);
    RUN_PROP("each_call_returns_independent_storage", prop_each_call_returns_independent_storage, 300);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
