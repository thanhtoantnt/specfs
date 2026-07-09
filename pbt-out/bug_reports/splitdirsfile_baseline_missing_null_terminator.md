# Bug: splitDirsFile does not terminate dirname output for empty paths (extent baseline)

## Status
Confirmed — failing theft property `terminates_uninitialized_output` against the real compiled `eval/extent/baseline/util.c::splitDirsFile` via CMake target `splitdirsfile_baseline_pbt_test`.

## Property
`splitdirsfile_baseline_pbt_test.terminates_uninitialized_output` (negative/error contract).

For all valid NUL-terminated paths with fewer than `MAX_PATH_LEN` non-empty slash-delimited components, `splitDirsFile(path, dirname, filename)` should leave `dirname` as a NULL-terminated vector of parent directory components. For zero-component paths (`""`, `"/"`, repeated slashes), that means `dirname[0] == NULL` and `filename` remains empty.

## Witness
```text
path="" components=[] -> dirname[0] remained non-NULL
path="/" components=[] -> dirname[0] remained non-NULL
path="//" components=[] -> dirname[0] remained non-NULL
```

The run found 6 failures in 300 trials for `terminates_uninitialized_output`. The other generated properties passed: component order/content, redundant slash equivalence, input immutability, and output non-aliasing.

## Root Cause
`eval/extent/baseline/util.c:61` returns immediately when tokenization finds no components:

```c
if(num == 0)
    return;
```

That path never writes `dirname[0] = NULL`, so callers that provide an uninitialized output vector receive a non-terminated vector. Non-empty paths do write a terminator after moving the final component into `filename`.

## Fix
Terminate the output vector before the empty-path return:

```c
if (num == 0) {
    dirname[0] = NULL;
    return;
}
```

## Reproduction
```bash
cmake -S pbt-native -B /tmp/specfs-splitdirsfile-baseline-pbt -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-splitdirsfile-baseline-pbt --target splitdirsfile_baseline_pbt_test -j2
ctest --test-dir /tmp/specfs-splitdirsfile-baseline-pbt -R '^splitdirsfile_baseline_pbt_test$' --output-on-failure
# terminates_uninitialized_output: FAIL
```
