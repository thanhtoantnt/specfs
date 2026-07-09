/*
 * NULL-inode safety property for inode_write() in
 * eval/extent/optimization/inode_management.c
 *
 * Focus: inode_write(NULL, buffer, len, offset) must return safely for
 * generated lengths, offsets, and non-NULL buffers.
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
#include "inode_management.h"

#define TEST_BUFFER_LEN 64U

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node;
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
    return NULL;
}

char *malloc_buffer(unsigned len)
{
    (void)len;
    return NULL;
}

unsigned int hash_func(char *str)
{
    (void)str;
    return 0;
}

struct null_inode_write_case {
    unsigned len;
    unsigned offset;
    char buffer[TEST_BUFFER_LEN];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static unsigned boundary_unsigned(struct theft *t)
{
    switch (theft_random_choice(t, 8)) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return bounded_choice(t, PAGE_SIZE);
    case 3:
        return MAX_FILE_SIZE - bounded_choice(t, PAGE_SIZE);
    case 4:
        return MAX_FILE_SIZE;
    case 5:
        return MAX_FILE_SIZE + bounded_choice(t, PAGE_SIZE);
    case 6:
        return UINT32_MAX - bounded_choice(t, PAGE_SIZE);
    default:
        return bounded_choice(t, MAX_FILE_SIZE + PAGE_SIZE);
    }
}

static enum theft_alloc_res null_inode_write_case_alloc_cb(struct theft *t,
                                                           void *env,
                                                           void **instance)
{
    (void)env;
    struct null_inode_write_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    tc->len = boundary_unsigned(t);
    tc->offset = boundary_unsigned(t);
    for (unsigned i = 0; i < TEST_BUFFER_LEN; i++) {
        tc->buffer[i] = (char)theft_random_choice(t, 256);
    }

    *instance = tc;
    return THEFT_ALLOC_OK;
}

static void null_inode_write_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash null_inode_write_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct null_inode_write_case));
}

static void null_inode_write_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct null_inode_write_case *tc = instance;
    fprintf(f, "{len=%u, offset=%u}", tc->len, tc->offset);
}

static struct theft_type_info null_inode_write_case_info = {
    .alloc = null_inode_write_case_alloc_cb,
    .free = null_inode_write_case_free_cb,
    .hash = null_inode_write_case_hash_cb,
    .print = null_inode_write_case_print_cb,
};

static enum theft_trial_res prop_null_inode_does_not_crash(struct theft *t, void *arg1)
{
    (void)t;
    const struct null_inode_write_case *tc = arg1;
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return THEFT_TRIAL_ERROR;
    }

    if (pid == 0) {
        inode_write(NULL, tc->buffer, tc->len, tc->offset);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return THEFT_TRIAL_ERROR;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "NULL inode crashed with signal %d for len=%u offset=%u\n",
                    WTERMSIG(status), tc->len, tc->offset);
        } else {
            fprintf(stderr, "NULL inode exited abnormally: status=%d len=%u offset=%u\n",
                    status, tc->len, tc->offset);
        }
        return THEFT_TRIAL_FAIL;
    }

    return THEFT_TRIAL_PASS;
}

int main(void)
{
    struct theft_run_config cfg = {
        .name = "null_inode_does_not_crash",
        .prop1 = prop_null_inode_does_not_crash,
        .type_info = { &null_inode_write_case_info },
        .trials = 200,
        .seed = theft_seed_of_time(),
    };

    printf("inode_write extent NULL-inode property-based tests:\n");
    enum theft_run_res res = theft_run(&cfg);
    printf("  [%s] null_inode_does_not_crash\n",
           res == THEFT_RUN_PASS ? "PASS" : "FAIL");
    return res == THEFT_RUN_PASS ? 0 : 1;
}
