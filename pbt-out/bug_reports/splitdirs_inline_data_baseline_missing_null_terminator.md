# Bug: splitDirs does not terminate dirname output (inline_data baseline)

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/inline_data/baseline/util.c::splitDirs` via CMake target `splitdirs_inline_data_baseline_pbt_test`.

## Property
`splitdirs_inline_data_baseline_pbt_test.terminates_uninitialized_output` (contract oracle).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirs(path, dirname)` should write a NULL sentinel at `dirname[component_count]`. The API returns a `char *dirname[]` vector consumed by NULL-terminated walkers such as `free_dirs`, so the vector must be terminated by `splitDirs` rather than relying on prior caller zero-initialization.

## Counterexample

```text
path="" components=[] -> dirname[0] remained non-NULL
```

The run found repeated failures in `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause

`eval/inline_data/baseline/util.c:77` initializes `num = 0` and appends each token into `dirname[num]`, but after the loop it only frees the temporary input buffer and returns. It never assigns `dirname[num] = NULL` after the last component.

## Suggested Fix

After the tokenization loop, write the sentinel before returning:

```c
dirname[num] = NULL;
```

## Reproduction

```bash
cmake -S pbt-native -B /tmp/specfs-pbt-splitdirs-inline-data-baseline -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-splitdirs-inline-data-baseline --target splitdirs_inline_data_baseline_pbt_test -j2
/tmp/specfs-pbt-splitdirs-inline-data-baseline/splitdirs_inline_data_baseline_pbt_test
```

Expected current result: `terminates_uninitialized_output` fails.
