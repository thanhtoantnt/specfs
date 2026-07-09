# Bug: baseline extent inode_truncate accepts out-of-range sizes

**Law:** `inode_truncate(node, size)` must reject sizes greater than `MAX_FILE_SIZE`; an invalid truncate request must leave `node->size` unchanged and must not call low-level allocation, clear, or write operations.

**Impact:** Out-of-range truncate sizes can grow an inode beyond the filesystem maximum and request allocation/clear ranges outside the valid file-size domain.

**Function:** `inode_truncate` in `eval/extent/baseline/inode_management.c`

**Detected by:** Negative/Error Contract property `out_of_range_size_is_rejected` in `pbt-native/inode_truncate_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{old_size=33554432, requested_size=33554463}` with `MAX_FILE_SIZE=33554432`.

**Expected:** Leave `node->size` unchanged and make zero low-level calls when `requested_size > MAX_FILE_SIZE`.

**Actual:** Sets `node->size` to the out-of-range size and calls `file_allocate(node->file, old_size, requested_size - old_size)` and `clear_file(node, old_size, requested_size - old_size)`.

**Severity:** High

**Regression test:** `pbt-native/inode_truncate_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-build --target inode_truncate_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-build/inode_truncate_baseline_pbt_test
```

## Root Cause

`inode_truncate` does not validate `size` against `MAX_FILE_SIZE` before computing `size - node->size`, calling low-level file operations, and assigning `node->size = size`.
