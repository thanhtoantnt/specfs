/*
 * Deterministic regression test for splitDirs() in
 * eval/inline_data/optimization/util.c
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "util.h"

int
main(void)
{
    char *dirs[MAX_PATH_LEN];

    for (size_t i = 0; i < MAX_PATH_LEN; i++) {
        dirs[i] = (char *)0x1;
    }

    splitDirs("/", dirs);

    assert(dirs[0] == NULL);
    return 0;
}
