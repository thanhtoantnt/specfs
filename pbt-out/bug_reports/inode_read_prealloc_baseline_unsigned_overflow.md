# Bug: inode_read fails to clamp when offset plus len overflows
**Law:** For all valid inodes with `offset < node->size`, `inode_read` must return and delegate exactly `min(len, node->size - offset)` bytes without relying on overflowing unsigned addition.
**Impact:** Large `len` values can make `offset + len` wrap below `node->size`, so `inode_read` requests and reports billions of bytes instead of the remaining file length.
**Function:** `inode_read`
**Detected by:** Algebraic/reference model over unsigned boundary inputs
**Minimal input:** `size=5741, offset=2053, len=4294967292` from `num_is_clamped_to_file_size`
**Expected:** `ret->num == 3688` and `file_read` called with `len == 3688`.
**Actual:** `ret->num == 4294967292` and `file_read` called with `len == 4294967292`.
**Severity:** high
**Regression test:** `pbt-native/inode_read_prealloc_baseline_pbt_test.c`
