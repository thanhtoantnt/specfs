# Bug: inode_read allocates and delegates zero-length in-range reads
**Law:** For all valid inodes, if `len == 0`, `inode_read(node, len, offset)` returns `num == 0`, `buf == NULL`, and does not call `malloc_buffer` or `file_read`.
**Impact:** Callers receive a non-NULL buffer for an empty read and the implementation performs unnecessary allocation/delegation, violating the empty-read contract used by sibling inode_read PBT suites.
**Function:** `inode_read`
**Detected by:** Algebraic/reference model plus negative contract
**Minimal input:** `size=994, offset=317, len=0` from `zero_len_returns_empty_without_allocating`
**Expected:** `ret->num == 0`, `ret->buf == NULL`, `file_read` not called, `malloc_buffer` not called.
**Actual:** `ret->num == 0`, `ret->buf != NULL`, `file_read` called once, `malloc_buffer` called once.
**Severity:** medium
**Regression test:** `pbt-native/inode_read_prealloc_baseline_pbt_test.c`
