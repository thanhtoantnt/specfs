# Bug: rbtree baseline inode_truncate accepts out-of-range sizes

**Law:** `inode_truncate(node, size)` must reject sizes greater than `MAX_FILE_SIZE`; an invalid truncate request must leave `node->size` unchanged and must not invoke low-level storage operations.

**Impact:** Out-of-range truncate sizes can grow an inode beyond the filesystem maximum, making `node->size` inconsistent with the valid low-level file address space.

**Function:** `inode_truncate` in `eval/rbtree/baseline/inode_management.c`

**Detected by:** Negative/Error Contract property `out_of_range_size_is_rejected` in `pbt-native/inode_truncate_rbtree_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{old_size=6240516, requested_size=33554531}` with `MAX_FILE_SIZE=33554432`.

**Expected:** Leave `node->size == old_size` and make zero low-level calls when `requested_size > MAX_FILE_SIZE`.

**Actual:** Sets `node->size` to the out-of-range `requested_size`.

**Severity:** High

**Regression test:** `pbt-native/inode_truncate_rbtree_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build-rbtree-baseline-truncate
cmake --build /tmp/specfs-pbt-build-rbtree-baseline-truncate --target inode_truncate_rbtree_baseline_pbt_test -j2
/tmp/specfs-pbt-build-rbtree-baseline-truncate/inode_truncate_rbtree_baseline_pbt_test
```

## Root Cause

`inode_truncate` does not validate `size <= MAX_FILE_SIZE` before assigning `node->size = size` in the growth branch.
