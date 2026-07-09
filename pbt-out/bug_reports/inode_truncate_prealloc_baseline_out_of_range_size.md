# Bug: pre_alloc baseline inode_truncate accepts out-of-range sizes

**Law:** `inode_truncate(node, size)` must reject `size > MAX_FILE_SIZE`; an invalid truncate request must leave `node->size` unchanged and must not issue low-level file operations.

**Impact:** Out-of-range truncate sizes can make inode metadata exceed the filesystem maximum file size, causing later reads/writes and size accounting to operate outside the supported domain.

**Function:** `inode_truncate` in `eval/pre_alloc/baseline/inode_management.c`

**Detected by:** Negative/Error Contract property `out_of_range_size_is_rejected` in `pbt-native/inode_truncate_prealloc_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{old_size=33554432, requested_size=33555690}` with `MAX_FILE_SIZE=33554432`.

**Expected:** Leave `node->size` unchanged and make no low-level calls when `requested_size > MAX_FILE_SIZE`.

**Actual:** Assigns `node->size = requested_size` for out-of-range growth.

**Severity:** High

**Regression test:** `pbt-native/inode_truncate_prealloc_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B pbt-native/build -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build --target inode_truncate_prealloc_baseline_pbt_test -j$(nproc)
./pbt-native/build/inode_truncate_prealloc_baseline_pbt_test
```

## Root Cause

The implementation does not validate `size <= MAX_FILE_SIZE` before assigning `node->size = size` in the growth branch.
