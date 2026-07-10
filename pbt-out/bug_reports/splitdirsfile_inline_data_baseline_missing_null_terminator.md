# Bug: splitDirsFile inline_data baseline does not terminate dirname output

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/inline_data/baseline/util.c::splitDirsFile`.

## Property
`splitdirsfile_inline_data_baseline_pbt_test.terminates_uninitialized_output` (contract oracle).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirsFile(path, dirname, filename)` should write a NULL sentinel at the end of the `dirname` vector. Callers such as `free_dirs` consume `dirname` as a NULL-terminated vector, so `splitDirsFile` must terminate it instead of relying on prior caller zero-initialization.

## Witness
```text
path="/////" components=[] -> dirname[0] remained non-NULL
path="" components=[] -> dirname[0] remained non-NULL
path="///" components=[] -> dirname[0] remained non-NULL
path="////" components=[] -> dirname[0] remained non-NULL
path="/" components=[] -> dirname[0] remained non-NULL
path="//" components=[] -> dirname[0] remained non-NULL
```

The run found 6 failures in 300 trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/inline_data/baseline/util.c:44` tokenizes the path and stores each component in `dirname[num]`, but when there are no non-empty components it returns without assigning `dirname[0] = NULL`. For non-empty paths it relies on freeing the final component slot and setting that slot to NULL, so the zero-component path is the exposed contract violation.

## Fix
Terminate the output vector before the empty-path return path as well, for example by setting `dirname[num] = NULL` after tokenization and before `if (num == 0) return;`.

## Reproduction
```bash
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target splitdirsfile_inline_data_baseline_pbt_test
ctest --test-dir pbt-native/build -R '^splitdirsfile_inline_data_baseline_pbt_test$' --output-on-failure
# terminates_uninitialized_output: FAIL
```
