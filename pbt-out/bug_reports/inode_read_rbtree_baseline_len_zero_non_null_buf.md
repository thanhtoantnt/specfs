# Bug: rbtree baseline inode_read returns non-NULL buffer for zero-length read

**Law:** For an empty read range (`offset >= node->size` or `len == 0`), `inode_read` must return `ret->num == 0` and `ret->buf == NULL` without delegating to `file_read`.

**Impact:** Callers receive a heap pointer even though no bytes were read, violating the documented ownership contract that only non-NULL data buffers need freeing for returned bytes.

**Function:** `inode_read` in `eval/rbtree/baseline/inode_management.c`

**Detected by:** Negative/error contract — theft property `zero_len_returns_empty_without_allocating`

**Minimal input:** `node.size = 2`, `offset = 1`, `len = 0` (representative theft witness from the run: `{size=4038, offset=3881, len=0}`)

**Expected:** `inode_read(&node, 0, offset)` returns a non-NULL `read_ret` with `num == 0`, `buf == NULL`, no `malloc_buffer` call, and no `file_read` call.

**Actual:** The implementation computes `actual_len = 0`, calls `malloc_buffer(0)`, delegates to `file_read(..., len=0, ...)`, stores the returned heap pointer in `ret->buf`, and returns `num == 0` with `buf != NULL`.

**Severity:** Low

**Regression test:** `pbt-native/inode_read_rbtree_baseline_pbt_test.c` property `zero_len_returns_empty_without_allocating`

## Evidence

- Spec: `sysspec/specfs/inode/inode_read.spec` Case 1 says empty reads (`offset >= node->size` or `len == 0`) return `ret->num = 0` and `ret->buf` is `NULL`.
- Source: `eval/rbtree/baseline/inode_management.c` allocates `malloc_buffer(actual_len)` even when `actual_len == 0`, then assigns that pointer to `ret->buf`.
- Run: `ctest -R '^inode_read_rbtree_baseline_pbt_test$' --output-on-failure` reports `FAIL zero_len_returns_empty_without_allocating`.

## Suggested Fix

Return the empty-read result before allocating a buffer whenever `len == 0` or the computed `actual_len == 0`.
