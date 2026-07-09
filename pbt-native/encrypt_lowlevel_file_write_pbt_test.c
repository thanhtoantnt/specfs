/*
 * Property-based tests for file_write() in
 * eval/loc/gen/encrypt/lowlevel_file.c
 *
 * Standalone harness: supplies the minimal inode/index table model and a
 * reversible encrypt/decrypt oracle so the generated low-level file code can
 * be tested without the rest of specfs.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PG_SIZE 64U
#define MAX_PAGES 16U
#define MAX_WRITE_BYTES 192U
#define MAX_CASE_PAGES 4U

struct inode {
    unsigned char key[16];
};

struct indextb {
    struct inode *parent_inode;
    void *index[MAX_PAGES];
};

void *encrypt_block(const void *plaintext, const unsigned char *key);
void *decrypt_block(const void *ciphertext, const unsigned char *key);
void free_block(void *block);

/* Include the target after defining its dependencies. */
#include "../eval/loc/gen/encrypt/lowlevel_file.c"

static int g_encrypt_fail_countdown = -1;
static int g_decrypt_fail_countdown = -1;

static void *crypt_copy(const void *src, const unsigned char *key)
{
    unsigned char *out = malloc(PG_SIZE);
    if (out == NULL) return NULL;
    unsigned char k = key != NULL ? key[0] : 0U;
    const unsigned char *in = src;
    for (unsigned i = 0; i < PG_SIZE; i++) {
        out[i] = (unsigned char)(in[i] ^ k ^ 0xA7U);
    }
    return out;
}

void *encrypt_block(const void *plaintext, const unsigned char *key)
{
    if (g_encrypt_fail_countdown == 0) return NULL;
    if (g_encrypt_fail_countdown > 0) g_encrypt_fail_countdown--;
    return crypt_copy(plaintext, key);
}

void *decrypt_block(const void *ciphertext, const unsigned char *key)
{
    if (g_decrypt_fail_countdown == 0) return NULL;
    if (g_decrypt_fail_countdown > 0) g_decrypt_fail_countdown--;
    return crypt_copy(ciphertext, key);
}

void free_block(void *block)
{
    free(block);
}

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char key0;
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

    wc->page_count = 1U + bounded_choice(t, MAX_CASE_PAGES);
    wc->start_page = bounded_choice(t, MAX_PAGES - wc->page_count);

    switch (theft_random_choice(t, 5)) {
    case 0: wc->page_off = 0U; break;
    case 1: wc->page_off = 1U; break;
    case 2: wc->page_off = PG_SIZE - 1U; break;
    case 3: wc->page_off = PG_SIZE / 2U; break;
    default: wc->page_off = bounded_choice(t, PG_SIZE); break;
    }

    unsigned available = wc->page_count * PG_SIZE - wc->page_off;
    unsigned max_len = available < MAX_WRITE_BYTES ? available : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);
    wc->key0 = (unsigned char)theft_random_choice(t, 256);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256);
    }

    *instance = wc;
    return THEFT_ALLOC_OK;
}

static void write_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash write_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct write_case));
}

static void write_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct write_case *wc = instance;
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u, key0=%u}",
            wc->start_page, wc->page_count, wc->page_off, wc->len, wc->key0);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static void init_inode(struct inode *node, unsigned char key0)
{
    memset(node, 0, sizeof(*node));
    node->key[0] = key0;
}

static void init_table(struct indextb *tb, struct inode *node)
{
    memset(tb, 0, sizeof(*tb));
    tb->parent_inode = node;
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static unsigned case_region_bytes(const struct write_case *wc)
{
    return wc->page_count * PG_SIZE;
}

static void cleanup_table(struct indextb *tb)
{
    for (unsigned i = 0; i < MAX_PAGES; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
}

static int set_plain_page(struct indextb *tb, const struct inode *node, unsigned page,
                          unsigned char fill)
{
    unsigned char plain[PG_SIZE];
    memset(plain, fill, sizeof(plain));
    tb->index[page] = encrypt_block(plain, node->key);
    return tb->index[page] != NULL;
}

static enum theft_trial_res prop_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, wc->key0);
    init_table(&tb, &node);

    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) return THEFT_TRIAL_ERROR;
    memset(out, 0xCC, wc->len);
    file_read(&tb, offset, wc->len, (char *)out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) fprintf(stderr, "roundtrip mismatch: offset=%u len=%u\n", offset, wc->len);

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_write_zero_fills_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, wc->key0);
    init_table(&tb, &node);

    unsigned region = case_region_bytes(wc);
    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, region);
    file_read(&tb, wc->start_page * PG_SIZE, region, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned abs_off = wc->start_page * PG_SIZE + i;
        unsigned char expected = 0U;
        if (abs_off >= offset && abs_off < offset + wc->len) {
            expected = wc->data[abs_off - offset];
        }
        if (out[i] != expected) {
            fprintf(stderr, "sparse mismatch at region byte %u: got=%u expected=%u\n",
                    i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_existing_blocks_preserve_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, wc->key0);
    init_table(&tb, &node);

    for (unsigned p = 0; p < wc->page_count; p++) {
        if (!set_plain_page(&tb, &node, wc->start_page + p, (unsigned char)(0x30U + p))) {
            cleanup_table(&tb);
            return THEFT_TRIAL_ERROR;
        }
    }

    unsigned offset = case_offset(wc);
    unsigned region = case_region_bytes(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    file_read(&tb, wc->start_page * PG_SIZE, region, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned abs_off = wc->start_page * PG_SIZE + i;
        unsigned page_delta = i / PG_SIZE;
        unsigned char expected = (unsigned char)(0x30U + page_delta);
        if (abs_off >= offset && abs_off < offset + wc->len) {
            expected = wc->data[abs_off - offset];
        }
        if (out[i] != expected) {
            fprintf(stderr, "preservation mismatch at region byte %u: got=%u expected=%u\n",
                    i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/*
 * Spec contract (file_clear.c): file_write(node, start, len, NULL)
 * must ZERO-FILL the region [start, start+len), not be a no-op.
 *
 * The encrypt variant rejects NULL data at line 87:
 *   if (len == 0 || data == NULL || ...) return;
 * This makes clear_file a silent no-op -- the region is NOT zeroed.
 * The property below asserts the SPEC contract (zero-fill), not the
 * buggy implementation (no-op).
 */
static enum theft_trial_res
prop_null_data_zero_fills(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, wc->key0);
    init_table(&tb, &node);

    /* Pre-fill the target page with non-zero data. */
    if (!set_plain_page(&tb, &node, wc->start_page, 0x5AU)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    /* Call file_write with NULL data -- per spec, should zero-fill. */
    file_write(&tb, case_offset(wc), wc->len, NULL);

    /* Read back -- should be all zeros if spec is honored. */
    unsigned char readback[PG_SIZE * 2];
    unsigned read_len = wc->len;
    if (read_len > sizeof(readback)) read_len = sizeof(readback);
    file_read(&tb, case_offset(wc), read_len, (char *)readback);

    int ok = 1;
    for (unsigned i = 0; i < read_len; i++) {
        if (readback[i] != 0) {
            fprintf(stderr, "FAIL: NULL-data write did not zero-fill: byte %u = 0x%02x\n",
                    i, readback[i]);
            ok = 0;
            break;
        }
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_encrypt_failure_preserves_existing_block(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, wc->key0);
    init_table(&tb, &node);

    if (!set_plain_page(&tb, &node, wc->start_page, 0xC3U)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char before[PG_SIZE];
    file_read(&tb, wc->start_page * PG_SIZE, PG_SIZE, (char *)before);

    g_encrypt_fail_countdown = 0;
    file_write(&tb, wc->start_page * PG_SIZE, wc->len, (const char *)wc->data);
    g_encrypt_fail_countdown = -1;

    unsigned char after[PG_SIZE];
    file_read(&tb, wc->start_page * PG_SIZE, PG_SIZE, (char *)after);
    int ok = memcmp(before, after, PG_SIZE) == 0;
    if (!ok) fprintf(stderr, "encrypt failure corrupted existing block\n");

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                  \
        struct theft_run_config cfg = {                                   \
            .name = name_,                                                \
            .prop1 = prop_,                                               \
            .type_info = { &write_case_info },                            \
            .trials = trials_,                                            \
            .seed = theft_seed_of_time(),                                 \
        };                                                                \
        enum theft_run_res res = theft_run(&cfg);                         \
        printf("  [%s] %s\n",                                           \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                            \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("encrypt lowlevel file_write property-based tests:\n");
    RUN_PROP("roundtrip_matches_input", prop_roundtrip_matches_input, 300);
    RUN_PROP("sparse_write_zero_fills_unwritten_bytes", prop_sparse_write_zero_fills_unwritten_bytes, 300);
    RUN_PROP("existing_blocks_preserve_unwritten_bytes", prop_existing_blocks_preserve_unwritten_bytes, 300);
    RUN_PROP("null_data_zero_fills", prop_null_data_zero_fills, 100);
    RUN_PROP("encrypt_failure_preserves_existing_block", prop_encrypt_failure_preserves_existing_block, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
