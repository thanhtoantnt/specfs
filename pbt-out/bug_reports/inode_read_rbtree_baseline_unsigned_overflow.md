# Bug: rbtree baseline inode_read fails to clamp reads when offset + len overflows

**Law:** For non-empty reads with `offset < node->size`, `ret->num` and the delegated `file_read` length must equal `min(len, node->size - offset)` using overflow-safe arithmetic.

**Impact:** A large `len` can wrap `offset + len` below `node->size`, causing `inode_read` to request and report billions of bytes instead of the few bytes remaining in the file. In production this can trigger excessive allocation, invalid low-level I/O, or incorrect byte counts returned to callers.

**Function:** `inode_read` in `eval/rbtree/baseline/inode_management.c`

**Detected by:** Algebraic/reference model — theft properties `num_is_clamped_to_file_size` and `nonempty_reads_delegate_exact_range`

**Minimal input:** `node.size = 2`, `offset = 1`, `len = UINT_MAX`. Expected `ret->num == 1`; current code returns/delegates `UINT_MAX`. Representative theft witnesses include `{size=455, offset=271, len=4294967295}` with expected `184` but actual `4294967295`.

**Expected:** The implementation clamps using `remaining = node->size - offset` and `actual_len = min(len, remaining)`, then allocates/delegates exactly `actual_len` bytes.

**Actual:** The implementation tests `if (offset + len > file_size)` in unsigned arithmetic. When `offset + len` wraps, the condition is false and `actual_len` remains the attacker-controlled huge `len`.

**Severity:** High

**Regression test:** `pbt-native/inode_read_rbtree_baseline_pbt_test.c` properties `num_is_clamped_to_file_size` and `nonempty_reads_delegate_exact_range`

## Evidence

- Spec: `sysspec/specfs/inode/inode_read.spec` Case 2 requires `ret->num = min(len, node->size - offset)`.
- Source: `eval/rbtree/baseline/inode_management.c` computes `if (offset + len > file_size)` instead of comparing `len` against `file_size - offset`.
- Run: `ctest -R '^inode_read_rbtree_baseline_pbt_test$' --output-on-failure` reports failures where `num` and `file_read` length equal `4294967295` instead of the remaining file size.

## Suggested Fix

After checking `offset < file_size`, compute `unsigned remaining = file_size - offset; unsigned actual_len = len < remaining ? len : remaining;` and avoid any `offset + len` comparison.
