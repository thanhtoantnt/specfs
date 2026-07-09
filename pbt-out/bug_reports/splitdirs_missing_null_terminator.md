# Bug: splitDirs does not terminate dirname output

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/delay_alloc/optimization/util.c::splitDirs`.

## Property
`splitdirs_pbt_test2.terminates_uninitialized_output` (contract oracle).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirs(path, dirname)` should write a NULL sentinel at `dirname[component_count]`. The API returns a `char *dirname[]` vector consumed by NULL-terminated walkers such as `free_dirs`, so the vector must be terminated by the function rather than relying on prior caller zero-initialization.

## Witness
```text
path="/////" components=[] -> dirname[0] remained non-NULL
path="IIqa]V//" components=["IIqa]V"] -> dirname[1] remained non-NULL
```

The run found 192 failures in 200 trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/delay_alloc/optimization/util.c:77` initializes `num = 0` and appends each token into `dirname[num]`, but after the loop it only frees the temporary input buffer and returns. Unlike `splitDirsFile`, it never assigns `dirname[num] = NULL` after the last component.

## Fix
Terminate the output vector before returning:

```c
free(input);
dirname[num] = NULL;
return;
```

## Reproduction
```bash
cmake -S pbt-native -B /tmp/specfs-splitdirs-pbt2 -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-splitdirs-pbt2 --target splitdirs_pbt_test2 -j2
ctest --test-dir /tmp/specfs-splitdirs-pbt2 -R '^splitdirs_pbt_test2$' --output-on-failure
# terminates_uninitialized_output: FAIL
```
