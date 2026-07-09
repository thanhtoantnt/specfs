# Bug: inline_data baseline inode_read returns non-NULL buffer for zero-length read

**Law:** For an empty read range (`offset >= node->size` or `len == 0`), `inode_read` must return `ret->num == 0`, `ret->buf == NULL`, and avoid low-level file I/O.

**Impact:** Callers receive a heap pointer and the low-level read path is invoked even though no bytes were read, violating the documented empty-read result and causing unnecessary ownership work for a zero-byte read.

**Function:** `inode_read` in `eval/inline_data/baseline/inode_management.c`

**Detected by:** Algebraic invariant / negative-contract oracle — theft property `empty_reads_return_null_buffer`

**Minimal input:** `node.size > 0`, `offset < node.size`, `len = 0`. Representative theft witness: `{size=4611, offset=2465, len=0}`.

**Expected:** Return a non-NULL `read_ret` with `num == 0`, `buf == NULL`, no `malloc_buffer` call, and no `file_read` call.

**Actual:** The implementation only checks `offset >= node->size`, then computes `actual_len = 0`, calls `malloc_buffer(0)`, calls `file_read(..., len=0, ...)`, and returns `num == 0` with `buf != NULL`.

**Severity:** Low

**Regression test:** `pbt-native/inode_read_inline_data_baseline_pbt_test.c` property `empty_reads_return_null_buffer`

## Evidence

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-inode-read-inline-baseline -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-inode-read-inline-baseline --target inode_read_inline_data_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-inode-read-inline-baseline/inode_read_inline_data_baseline_pbt_test
```

The theft run reports `FAIL empty_reads_return_null_buffer` with witnesses where `len=0`, `offset < size`, `num=0`, `buf != NULL`, `file_calls=1`, and `malloc_calls=1`.

## Suggested Fix

Return the empty-read result before allocating or delegating to `file_read` whenever `len == 0` or the computed `actual_len == 0`.
