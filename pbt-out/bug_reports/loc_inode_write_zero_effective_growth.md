# Bug: loc inode_write allocates and grows file when no byte is written
**Law:** A zero-effective write (`len == 0` or `offset >= MAX_FILE_SIZE`) must not change inode size or request allocation/clearing because no byte is written.
**Impact:** Calls that write zero bytes can unexpectedly grow the inode to `MAX_FILE_SIZE` and allocate/clear storage, changing persistent metadata for a no-op request.
**Function:** `inode_write`
**Detected by:** Algebraic Invariant property in `pbt-native/loc_inode_write_pbt_test.c`
**Minimal input:** `initial_size=173, offset=1089, len=0` with test `MAX_FILE_SIZE=512`
**Expected:** return `0`, keep `node->size == 173`, and call neither `file_allocate` nor `file_clear`.
**Actual:** requests `file_allocate(node, 173, 339)`, `file_clear(node, 173, 339)`, and grows `node->size` to `512`.
**Severity:** medium
**Regression test:** `pbt-native/loc_inode_write_pbt_test.c`
