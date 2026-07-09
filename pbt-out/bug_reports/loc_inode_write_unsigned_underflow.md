# Bug: loc inode_write writes positive length for overflowed out-of-range offset
**Law:** If `offset >= MAX_FILE_SIZE`, no byte can fit in the file, so `inode_write` must return 0 and must not issue a positive-length low-level `file_write`.
**Impact:** A write request far beyond the maximum file size can be treated as a positive-length write after unsigned `offset + len` overflow, forwarding an invalid offset to lower-level storage.
**Function:** `inode_write`
**Detected by:** Algebraic Invariant / Negative Contract property in `pbt-native/loc_inode_write_pbt_test.c`
**Minimal input:** `initial_size=432, offset=4294967232, len=302` with test `MAX_FILE_SIZE=512`
**Expected:** return `0`, leave size unchanged, and issue no positive-length `file_write`.
**Actual:** returns `302` and calls `file_write(node, 4294967232, 302, buffer)`.
**Severity:** high
**Regression test:** `pbt-native/loc_inode_write_pbt_test.c`
