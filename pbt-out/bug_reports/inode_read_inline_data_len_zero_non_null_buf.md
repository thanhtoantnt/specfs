# Bug: inline_data inode_read returns non-NULL buffer for zero-length read

**Law:** For an empty read range (`offset >= node->size` or `len == 0`), `inode_read` must return `ret->num == 0` and `ret->buf == NULL`.

**Impact:** Callers receive a heap pointer even though no bytes were read, violating the documented empty-read result and causing unnecessary ownership work for a zero-byte read.

**Function:** `inode_read` in `eval/inline_data/optimization/inode_management.c`

**Detected by:** Algebraic invariant / contract oracle — empty-read postcondition

**Minimal input:** `node.size = 1`, `offset = 0`, `len = 0` (representative theft witness: `{size=3120, offset=936, len=0}`)

**Expected:** `inode_read(&node, 0, 0)` returns a non-NULL `read_ret` with `num == 0` and `buf == NULL`.

**Actual:** The implementation only checks `offset >= node->size`, then computes `actual_len = 0`, calls `malloc_buffer(0)`, calls `file_read(..., len=0, ...)`, and returns `num == 0` with `buf != NULL`.

**Severity:** Low

**Regression test:** `pbt-native/inode_read_inline_data_pbt_test.c` property `empty_reads_return_null_buffer`

## Evidence

- Spec: `sysspec/specfs/inode/inode_read.spec` Case 1 says empty reads (`offset ≥ node->size` or `len == 0`) return `ret->num = 0` and `ret->buf` is `NULL`.
- Source: `eval/inline_data/optimization/inode_management.c` allocates `malloc_buffer(actual_len)` when `len == 0` and `offset < node->size`.
- Run: `./pbt-native/build/inode_read_inline_data_pbt_test` reports `FAIL empty_reads_return_null_buffer`; the other four properties pass.

## Suggested Fix

Return the empty-read result before allocating or delegating to `file_read` whenever `len == 0` or the computed `actual_len == 0`.
