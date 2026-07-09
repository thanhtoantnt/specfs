# Bug: rbtree baseline inode_write accepts out-of-range offsets

**Law:** `inode_write(node, buffer, len, offset)` must clamp writes to `MAX_FILE_SIZE`; when `offset >= MAX_FILE_SIZE`, the effective written length is `0` and no low-level write should occur.

**Impact:** Out-of-range offsets can produce wrapped non-zero write lengths and can update `node->size` beyond `MAX_FILE_SIZE`, exposing invalid low-level write ranges beyond the file-size limit.

**Function:** `inode_write` in `eval/rbtree/baseline/inode_management.c`

**Detected by:** Negative/Error Contract and Algebraic Invariant properties in `pbt-native/inode_write_rbtree_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{initial_size=32151861, offset=33554920, len=149}` with `MAX_FILE_SIZE=33554432`.

**Expected:** Return `0`, leave `node->size` unchanged for out-of-range offsets, and do not call `file_write` with an out-of-range offset.

**Actual:** Returns a non-zero length such as `149`, calls `file_write` with `offset=33554920`, and can update `node->size` beyond `MAX_FILE_SIZE`.

**Severity:** High

**Regression test:** `pbt-native/inode_write_rbtree_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-build --target inode_write_rbtree_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-build/inode_write_rbtree_baseline_pbt_test
```

## Root Cause

`inode_write` computes `MAX_FILE_SIZE - offset` using unsigned arithmetic before proving that `offset < MAX_FILE_SIZE`:

```c
unsigned max_write = MAX_FILE_SIZE - offset;
if (max_write < 0) {
    return 0;
}
```

Because `max_write` is unsigned, `max_write < 0` is always false. For `offset > MAX_FILE_SIZE`, the subtraction underflows to a large unsigned value, so the returned length and low-level write length are not clamped to zero.

## Suggested Fix

Check the offset before subtracting:

```c
if (offset >= MAX_FILE_SIZE) {
    return 0;
}
unsigned max_write = MAX_FILE_SIZE - offset;
```
