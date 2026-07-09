/*
 * Property-based tests for find() in
 * eval/extent/baseline/inode_management.c
 *
 * The user-facing directory operation code in
 * eval/extent/baseline/directory_operations.c calls this baseline find symbol.
 *
 * Oracle: algebraic/reference directory lookup contract. Stronger state-machine
 * testing is unnecessary because find is a read-only query: for any directory
 * table and filename, lookup must inspect only hash_func(name) % DIRTB_NUM,
 * return the first matching inode in that bucket, return NULL on misses/null
 * inputs, and leave the directory structure unchanged.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

#define MAX_NAME_LEN 24U
#define MAX_CHAIN_LEN 8U
#define MAX_OTHER_BUCKETS 6U

struct find_case {
    char query[MAX_NAME_LEN + 1U];
    char target_name[MAX_NAME_LEN + 1U];
    unsigned target_pos;
    unsigned chain_len;
    unsigned other_bucket_count;
    int include_target;
    int same_name_wrong_bucket;
};

struct dir_snapshot {
    struct entry *heads[DIRTB_NUM];
    struct entry *nexts[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    char *names[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    void *inums[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    unsigned entry_count;
};

void file_allocate(struct indextb *tb, unsigned offset, unsigned len)
{
    (void)tb;
    (void)offset;
    (void)len;
}

void file_write(struct indextb *tb, unsigned offset, unsigned len, const char *data)
{
    (void)tb;
    (void)offset;
    (void)len;
    (void)data;
}

void file_read(struct indextb *tb, unsigned offset, unsigned len, char *data)
{
    (void)tb;
    (void)offset;
    (void)len;
    (void)data;
}

void clear_file(struct inode *node, unsigned start, unsigned len)
{
    (void)node;
    (void)start;
    (void)len;
}

struct read_ret *malloc_readret(void)
{
    return malloc(sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    return malloc(len == 0U ? 1U : len);
}

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static void gen_name(struct theft *t, char *out, const char *prefix)
{
    unsigned prefix_len = (unsigned)strlen(prefix);
    unsigned suffix_len = 1U + bounded_choice(t, MAX_NAME_LEN - prefix_len);

    memcpy(out, prefix, prefix_len);
    for (unsigned i = 0; i < suffix_len; i++) {
        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
        out[prefix_len + i] = alphabet[bounded_choice(t, (unsigned)(sizeof(alphabet) - 1U))];
    }
    out[prefix_len + suffix_len] = '\0';
}

static void make_distinct_name(struct theft *t, char *out, const char *avoid, unsigned index)
{
    if (t == NULL) {
        snprintf(out, MAX_NAME_LEN + 1U, "x%u_%s", index, avoid);
        out[MAX_NAME_LEN] = '\0';
        if (strcmp(out, avoid) == 0) {
            snprintf(out, MAX_NAME_LEN + 1U, "x%u_alt", index);
            out[MAX_NAME_LEN] = '\0';
        }
        return;
    }

    do {
        char prefix[4];
        snprintf(prefix, sizeof(prefix), "x%u", index % 10U);
        gen_name(t, out, prefix);
    } while (strcmp(out, avoid) == 0);
}

static enum theft_alloc_res find_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct find_case *fc = malloc(sizeof(*fc));
    if (fc == NULL) return THEFT_ALLOC_ERROR;
    memset(fc, 0, sizeof(*fc));

    gen_name(t, fc->query, "q");
    if (theft_random_choice(t, 4) == 0) {
        make_distinct_name(t, fc->target_name, fc->query, 7U);
        fc->include_target = 0;
    } else {
        memcpy(fc->target_name, fc->query, sizeof(fc->target_name));
        fc->include_target = 1;
    }

    fc->chain_len = 1U + bounded_choice(t, MAX_CHAIN_LEN);
    fc->target_pos = bounded_choice(t, fc->chain_len);
    fc->other_bucket_count = bounded_choice(t, MAX_OTHER_BUCKETS + 1U);
    fc->same_name_wrong_bucket = (int)theft_random_choice(t, 2);

    *instance = fc;
    return THEFT_ALLOC_OK;
}

static void find_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash find_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct find_case));
}

static void find_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct find_case *fc = instance;
    fprintf(f,
            "{query=\"%s\", target_name=\"%s\", include_target=%d, target_pos=%u, chain_len=%u, other_bucket_count=%u, same_name_wrong_bucket=%d}",
            fc->query, fc->target_name, fc->include_target, fc->target_pos,
            fc->chain_len, fc->other_bucket_count, fc->same_name_wrong_bucket);
}

static struct theft_type_info find_case_info = {
    .alloc = find_case_alloc_cb,
    .free = find_case_free_cb,
    .hash = find_case_hash_cb,
    .print = find_case_print_cb,
};

static unsigned bucket_for(char *name)
{
    return hash_func(name) % DIRTB_NUM;
}

static void init_inode(struct inode *node, unsigned tag)
{
    memset(node, 0, sizeof(*node));
    node->mode = FILE_MODE;
    node->maj = tag;
    node->min = tag ^ 0xA5A5U;
    node->size = tag * 17U;
}

static void link_chain(struct entry *entries, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        entries[i].next = i + 1U < count ? &entries[i + 1U] : NULL;
    }
}

static void build_directory(const struct find_case *fc, struct dirtb *dir,
                            struct entry *entries, char names[][MAX_NAME_LEN + 1U],
                            struct inode *inodes, struct inode **expected)
{
    memset(dir, 0, sizeof(*dir));
    *expected = NULL;

    unsigned query_bucket = bucket_for((char *)fc->query);
    for (unsigned i = 0; i < fc->chain_len; i++) {
        if (fc->include_target && i == fc->target_pos) {
            memcpy(names[i], fc->target_name, MAX_NAME_LEN + 1U);
            *expected = &inodes[i];
        } else {
            make_distinct_name(NULL, names[i], fc->query, i);
        }
        init_inode(&inodes[i], i + 1U);
        entries[i].name = names[i];
        entries[i].inum = &inodes[i];
    }
    link_chain(entries, fc->chain_len);
    dir->tb[query_bucket] = &entries[0];

    for (unsigned i = 0; i < fc->other_bucket_count; i++) {
        unsigned entry_index = fc->chain_len + i;
        unsigned bucket = (query_bucket + 1U + i) % DIRTB_NUM;
        if (fc->same_name_wrong_bucket && i == 0U) {
            memcpy(names[entry_index], fc->query, MAX_NAME_LEN + 1U);
        } else {
            make_distinct_name(NULL, names[entry_index], fc->query, entry_index);
        }
        init_inode(&inodes[entry_index], entry_index + 1U);
        entries[entry_index].name = names[entry_index];
        entries[entry_index].inum = &inodes[entry_index];
        entries[entry_index].next = dir->tb[bucket];
        dir->tb[bucket] = &entries[entry_index];
    }
}

static void snapshot_directory(const struct dirtb *dir, const struct entry *entries,
                               unsigned entry_count, struct dir_snapshot *snapshot)
{
    for (unsigned i = 0; i < DIRTB_NUM; i++) {
        snapshot->heads[i] = dir->tb[i];
    }
    snapshot->entry_count = entry_count;
    for (unsigned i = 0; i < entry_count; i++) {
        snapshot->nexts[i] = entries[i].next;
        snapshot->names[i] = entries[i].name;
        snapshot->inums[i] = entries[i].inum;
    }
}

static int directory_matches_snapshot(const struct dirtb *dir, const struct entry *entries,
                                      const struct dir_snapshot *snapshot)
{
    for (unsigned i = 0; i < DIRTB_NUM; i++) {
        if (dir->tb[i] != snapshot->heads[i]) return 0;
    }
    for (unsigned i = 0; i < snapshot->entry_count; i++) {
        if (entries[i].next != snapshot->nexts[i] || entries[i].name != snapshot->names[i] ||
            entries[i].inum != snapshot->inums[i]) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_lookup_matches_reference_bucket(struct theft *t, void *arg1)
{
    (void)t;
    const struct find_case *fc = arg1;
    struct dirtb dir;
    struct entry entries[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    char names[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U][MAX_NAME_LEN + 1U];
    struct inode inodes[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    struct inode *expected = NULL;

    build_directory(fc, &dir, entries, names, inodes, &expected);
    struct inode *actual = find(&dir, (char *)fc->query);

    if (actual != expected) {
        fprintf(stderr, "lookup mismatch: query=%s actual=%p expected=%p bucket=%u\n",
                fc->query, (void *)actual, (void *)expected, bucket_for((char *)fc->query));
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_same_name_in_wrong_bucket_is_ignored(struct theft *t, void *arg1)
{
    (void)t;
    const struct find_case *fc = arg1;
    struct dirtb dir;
    struct entry wrong_entry;
    struct inode wrong_inode;
    unsigned query_bucket = bucket_for((char *)fc->query);
    unsigned wrong_bucket = (query_bucket + 1U) % DIRTB_NUM;

    memset(&dir, 0, sizeof(dir));
    init_inode(&wrong_inode, 99U);
    wrong_entry.name = (char *)fc->query;
    wrong_entry.inum = &wrong_inode;
    wrong_entry.next = NULL;
    dir.tb[wrong_bucket] = &wrong_entry;

    struct inode *actual = find(&dir, (char *)fc->query);
    if (actual != NULL) {
        fprintf(stderr, "wrong-bucket entry was returned: query=%s bucket=%u wrong_bucket=%u\n",
                fc->query, query_bucket, wrong_bucket);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_find_is_read_only(struct theft *t, void *arg1)
{
    (void)t;
    const struct find_case *fc = arg1;
    struct dirtb dir;
    struct entry entries[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    char names[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U][MAX_NAME_LEN + 1U];
    struct inode inodes[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    struct inode *expected = NULL;
    struct dir_snapshot before;
    unsigned entry_count = fc->chain_len + fc->other_bucket_count;

    build_directory(fc, &dir, entries, names, inodes, &expected);
    snapshot_directory(&dir, entries, entry_count, &before);
    (void)find(&dir, (char *)fc->query);

    if (!directory_matches_snapshot(&dir, entries, &before)) {
        fprintf(stderr, "find mutated directory structure for query=%s\n", fc->query);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_null_inputs_return_null(struct theft *t, void *arg1)
{
    (void)t;
    const struct find_case *fc = arg1;
    struct dirtb dir;
    struct entry entries[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    char names[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U][MAX_NAME_LEN + 1U];
    struct inode inodes[MAX_CHAIN_LEN + MAX_OTHER_BUCKETS + 1U];
    struct inode *expected = NULL;

    build_directory(fc, &dir, entries, names, inodes, &expected);

    if (find(NULL, (char *)fc->query) != NULL || find(&dir, NULL) != NULL) {
        fprintf(stderr, "null input did not return NULL\n");
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                 \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &find_case_info },                           \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                         \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);       \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("baseline find property-based tests:\n");
    RUN_PROP("lookup_matches_reference_bucket", prop_lookup_matches_reference_bucket, 500);
    RUN_PROP("same_name_in_wrong_bucket_is_ignored", prop_same_name_in_wrong_bucket_is_ignored, 200);
    RUN_PROP("find_is_read_only", prop_find_is_read_only, 300);
    RUN_PROP("null_inputs_return_null", prop_null_inputs_return_null, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
