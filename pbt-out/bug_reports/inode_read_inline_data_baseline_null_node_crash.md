# Bug: inline_data baseline inode_read crashes on NULL inode

**Law:** `inode_read(NULL, len, offset)` must return `NULL` without invoking allocation or file I/O.

**Impact:** Callers that pass a NULL inode hit a segmentation fault instead of a safe error result, violating the file's memory-safety requirement to check pointers before use.

**Function:** `inode_read` in `eval/inline_data/baseline/inode_management.c`

**Detected by:** Negative/Error Contract oracle — theft property `null_node_returns_null_without_side_effects`

**Minimal input:** `node = NULL` with any generated `len` and `offset`. Representative theft witness: `{size=8192, offset=8191, len=5285}`.

**Expected:** Return `NULL` and perform no `malloc_buffer` or `file_read` calls.

**Actual:** The child process exits with status `139` (`SIGSEGV`) when `inode_read` dereferences `node->size` before checking `node`.

**Severity:** Medium

**Regression test:** `pbt-native/inode_read_inline_data_baseline_pbt_test.c` property `null_node_returns_null_without_side_effects`

## Evidence

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-inode-read-inline-baseline -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-inode-read-inline-baseline --target inode_read_inline_data_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-inode-read-inline-baseline/inode_read_inline_data_baseline_pbt_test
```

The theft run reports `FAIL null_node_returns_null_without_side_effects` and logs `status=139`.

## Suggested Fix

Check `node == NULL` at function entry and return `NULL` before allocating `read_ret` or dereferencing inode fields.
