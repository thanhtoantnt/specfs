# Bug: splitDirs inline_data does not terminate dirname output

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/inline_data/optimization/util.c::splitDirs`.

## Property
For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirs(path, dirname)` must write a `NULL` sentinel at `dirname[component_count]`.

The output is consumed as a NULL-terminated `char *dirname[]` vector by helpers such as `free_dirs`, so termination must be guaranteed by `splitDirs` and must not depend on caller pre-zeroing.

## Witness
```text
path="" components=[] -> dirname[0] remained non-NULL
path="//!3@k~f~`|NBBHq%O7iq[B-/" components=["!3@k~f~`|NBBHq%O7iq[B-"] -> dirname[1] remained non-NULL
```

The run found `197` failures in `200` trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/inline_data/optimization/util.c:77` appends each token into `dirname[num]`, then frees the temporary input buffer and returns. It never assigns `dirname[num] = NULL` after the final component.

## Suggested Fix
Terminate the output vector before returning:

```c
free(input);
dirname[num] = NULL;
return;
```

## Reproduction
```bash
cmake -S pbt-native -B /tmp/specfs-splitdirs-inline-data-pbt -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-splitdirs-inline-data-pbt --target splitdirs_inline_data_pbt_test -j2
ctest --test-dir /tmp/specfs-splitdirs-inline-data-pbt -R '^splitdirs_inline_data_pbt_test$' --output-on-failure
```

Expected current result: `terminates_uninitialized_output` fails.
