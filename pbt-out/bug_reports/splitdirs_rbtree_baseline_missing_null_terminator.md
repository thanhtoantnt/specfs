# Bug: splitDirs does not terminate dirname output in rbtree baseline util.c

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/rbtree/baseline/util.c::splitDirs`.

## Property
`splitdirs_rbtree_baseline_pbt_test.terminates_uninitialized_output` (contract oracle).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirs(path, dirname)` should write a NULL sentinel at `dirname[component_count]`. The API returns a `char *dirname[]` vector consumed by NULL-terminated walkers such as `free_dirs`, so the vector must be terminated by the function rather than relying on prior caller zero-initialization.

## Witness
```text
path="//*tp/" components=["*tp"] -> dirname[1] remained non-NULL
```

The run found repeated failures in 200 trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/rbtree/baseline/util.c` initializes `num = 0` and appends each token into `dirname[num]`, but after the loop it only frees the temporary input buffer and returns. Unlike `splitDirsFile`, it never assigns `dirname[num] = NULL` after the last component.

## Fix
Terminate the output vector before returning:

```c
free(input);
dirname[num] = NULL;
return;
```

## Reproduction
```bash
cmake -S pbt-native -B /tmp/specfs-pbt-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-build --target splitdirs_rbtree_baseline_pbt_test -j2
/tmp/specfs-pbt-build/splitdirs_rbtree_baseline_pbt_test
# terminates_uninitialized_output: FAIL
```
