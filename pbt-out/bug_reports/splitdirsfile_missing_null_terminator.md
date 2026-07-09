# Bug: splitDirsFile does not terminate dirname output

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/pre_alloc/optimization/util.c::splitDirsFile`.

## Property
`splitdirsfile_prealloc_pbt_test.terminates_uninitialized_output` (contract oracle).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirsFile(path, dirname, filename)` should write a NULL sentinel at `dirname[component_count]`. The API returns a `char *dirname[]` vector consumed by NULL-terminated walkers such as `free_dirs`, so the vector must be terminated by the function rather than relying on prior caller zero-initialization.

## Witness
```text
path="" components=[] -> dirname[0] remained non-NULL
path="///" components=[] -> dirname[0] remained non-NULL
path="IIqa]V//" components=["IIqa]V"] -> dirname[1] remained non-NULL
```

The run found 6 failures in 300 trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/pre_alloc/optimization/util.c:45` tokenizes the path and stores each component in `dirname[num]`, but after the loop it only copies the final token into `filename` and frees the last directory entry. It never assigns `dirname[num] = NULL` to terminate the vector.

## Fix
Terminate the output vector before returning:

```c
free(input);
dirname[num] = NULL;
return;
```

## Reproduction
```bash
cmake -S pbt-native -B /tmp/specfs-prealloc-splitdirs-pbt -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-prealloc-splitdirs-pbt --target splitdirsfile_prealloc_pbt_test -j2
ctest --test-dir /tmp/specfs-prealloc-splitdirs-pbt -R '^splitdirsfile_prealloc_pbt_test$' --output-on-failure
# terminates_uninitialized_output: FAIL
```
