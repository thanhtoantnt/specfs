# Bug: baseline extent inode_write accepts out-of-range offsets

**Law:** `inode_write(node, buffer, len, offset)` must clamp writes to `MAX_FILE_SIZE`; when `offset >= MAX_FILE_SIZE`, the effective written length is `0` and no low-level write should occur.

**Impact:** Out-of-range offsets can produce huge wrapped lengths and still call `file_write`, exposing invalid low-level write ranges beyond the file-size limit.

**Function:** `inode_write` in `eval/extent/baseline/inode_management.c`

**Detected by:** Negative/Error Contract and Algebraic Invariant properties in `pbt-native/inode_write_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{initial_size=10679782, offset=33554807, len=177}` with `MAX_FILE_SIZE=33554432`.

**Expected:** Return `0`, leave `node->size` unchanged for out-of-range offsets, and do not call `file_write` with an out-of-range offset.

**Actual:** Returns a wrapped non-zero length such as `4294966921` and calls `file_write` with `offset=33554807` and the wrapped length.

**Severity:** High

**Regression test:** `pbt-native/inode_write_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-build --target inode_write_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-build/inode_write_baseline_pbt_test
```

## Root Cause

`inode_write` computes `offset + len` in unsigned arithmetic and then uses `MAX_FILE_SIZE - offset` after the overflow-prone comparison. For `offset > MAX_FILE_SIZE`, `MAX_FILE_SIZE - offset` wraps to a large unsigned value, so the returned length and low-level write length are not clamped to zero.
