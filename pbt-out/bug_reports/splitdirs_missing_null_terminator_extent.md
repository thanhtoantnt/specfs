# Bug: splitDirs does not terminate dirname output (extent)

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/extent/optimization/util.c::splitDirs`.

## Property
`splitdirs_extent_pbt_test.terminates_uninitialized_output` (contract oracle).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirs(path, dirname)` should write a NULL sentinel at `dirname[component_count]`. The API returns a `char *dirname[]` vector consumed by NULL-terminated walkers such as `free_dirs`, so the vector must be terminated by the function rather than relying on prior caller zero-initialization.

## Witness
```text
path="" -> dirname[0] remained non-NULL
```

The run found 193 failures in 200 trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/extent/optimization/util.c:77` initializes `num = 0` and appends each token into `dirname[num]`, but after the loop it only frees the temporary input buffer and returns. Unlike `splitDirsFile`, it never assigns `dirname[num] = NULL` after the last component.

## Fix
Terminate the output vector before returning:

```c
free(input);
dirname[num] = NULL;
return;
```

## Reproduction
```bash
cmake -S pbt-native -B /tmp/specfs-splitdirs-extent-pbt -G 'Unix Makefiles'
cmake --build /tmp/specfs-splitdirs-extent-pbt --target splitdirs_extent_pbt_test -j2
ctest --test-dir /tmp/specfs-splitdirs-extent-pbt -R '^splitdirs_extent_pbt_test$' --output-on-failure
# terminates_uninitialized_output: FAIL
```
