# Bug: inode_read crashes on NULL inode

**Law:** `inode_read(NULL, len, offset)` must return `NULL` without invoking allocation or file I/O.
**Impact:** Callers that pass a NULL inode hit a segmentation fault instead of a safe error result, violating the file's memory-safety requirement to check pointers before use.
**Function:** `inode_read`
**Detected by:** Negative/Error Contract — theft property `null_node_returns_null_without_side_effects`
**Minimal input:** `node=NULL`, with generated witness `{size=2505, offset=7104, len=3783}`; the crash is independent of the generated inode size bytes because the inode pointer is NULL.
**Expected:** Return `NULL` and perform no `malloc_buffer` or `file_read` calls.
**Actual:** The child process exits with status `139` (`SIGSEGV`) when `inode_read` dereferences `node->size` before checking `node`.
**Severity:** medium
**Regression test:** `pbt-native/inode_read_baseline_pbt_test.c`
