# Bug: rbtree baseline inode_truncate growth skips clearing newly exposed bytes

**Law:** When `inode_truncate(node, size)` grows an inode (`size > node->size`), it must clear the newly exposed byte range `[old_size, size)` so subsequent reads observe zero-filled data rather than stale/uninitialized backing storage.

**Impact:** Growing a file by truncation can expose bytes that were never cleared through the low-level file path, violating filesystem zero-fill semantics for newly visible regions.

**Function:** `inode_truncate` in `eval/rbtree/baseline/inode_management.c`

**Detected by:** Algebraic invariant property `grow_clears_new_region` in `pbt-native/inode_truncate_rbtree_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{old_size=2, requested_size=7}`.

**Expected:** Set `node->size = requested_size` and call `clear_file(node, old_size, requested_size - old_size)` exactly once.

**Actual:** Sets `node->size = requested_size` and returns without calling `clear_file`.

**Severity:** High

**Regression test:** `pbt-native/inode_truncate_rbtree_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build-rbtree-baseline-truncate
cmake --build /tmp/specfs-pbt-build-rbtree-baseline-truncate --target inode_truncate_rbtree_baseline_pbt_test -j2
/tmp/specfs-pbt-build-rbtree-baseline-truncate/inode_truncate_rbtree_baseline_pbt_test
```

## Root Cause

The grow branch in `inode_truncate` only assigns `node->size = size` when `size >= old_size`; it does not clear the interval `[old_size, size)`.
