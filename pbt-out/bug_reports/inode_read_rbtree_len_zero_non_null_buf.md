# Bug: rbtree inode_read returns non-NULL buffer for zero-length read

**Law:** For an empty read range (`offset >= node->size` or `len == 0`), `inode_read` must return `ret->num == 0` and `ret->buf == NULL`.

**Impact:** Callers receive a heap pointer even though no bytes were read, violating the documented ownership contract that only non-NULL data buffers need freeing for returned bytes.

**Function:** `inode_read` in `eval/rbtree/optimization/inode_management.c`

**Detected by:** Negative/error contract — empty-read postcondition

**Minimal input:** `node.size = 1`, `offset = 0`, `len = 0` (representative theft witness: `{size=4993, offset=3740, len=0}`)

**Expected:** `inode_read(&node, 0, 0)` returns a non-NULL `read_ret` with `num == 0` and `buf == NULL`.

**Actual:** The implementation computes `actual_len = 0`, calls `malloc_buffer(0)`, stores the returned heap pointer in `ret->buf`, and returns `num == 0` with `buf != NULL`.

**Severity:** Low

**Regression test:** `pbt-native/inode_read_rbtree_pbt_test.c` property `empty_reads_return_null_buffer`

## Evidence

- Spec: `sysspec/specfs/inode/inode_read.spec` Case 1 says empty reads (`offset ≥ node->size` or `len == 0`) return `ret->num = 0` and `ret->buf` is `NULL`.
- Source: `eval/rbtree/optimization/inode_management.c` allocates `malloc_buffer(actual_len)` even when `actual_len == 0`, then assigns that pointer to `ret->buf`.
- Run: `./pbt-native/build/inode_read_rbtree_pbt_test` reports `FAIL empty_reads_return_null_buffer`; other properties pass.

## Suggested Fix

Return the empty-read result before allocating the buffer whenever `len == 0` or the computed `actual_len == 0`.
