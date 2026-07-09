# Bug: pre_alloc baseline inode_write accepts out-of-range offsets

**Law:** `inode_write(node, buffer, len, offset)` must clamp writes to `MAX_FILE_SIZE`; when `offset >= MAX_FILE_SIZE`, the effective written length is `0`, no low-level `file_write` should occur, and `node->size` must remain unchanged.

**Impact:** Out-of-range offsets can produce wrapped non-zero write lengths, call `file_write` beyond the file-size limit, and increase `node->size` past `MAX_FILE_SIZE` or to a wrapped high value.

**Function:** `inode_write` in `eval/pre_alloc/baseline/inode_management.c`

**Detected by:** Negative/Error Contract and Algebraic Invariant properties in `pbt-native/inode_write_prealloc_baseline_pbt_test.c`

**Minimal input:** A representative generated counterexample was `{initial_size=26990404, offset=4294966957, len=125}` with `MAX_FILE_SIZE=33554432`. A smaller deterministic witness is `initial_size=0`, `offset=MAX_FILE_SIZE + 1`, `len=1`, `buffer` non-NULL.

**Expected:** Return `0`, leave `node->size` unchanged, and do not call `file_write` for `offset >= MAX_FILE_SIZE`.

**Actual:** Returns a non-zero length and calls `file_write` with the invalid offset; the PBT run observed examples such as `actual=125 calls=1 size=4294967082 initial=26990404 offset=4294966957 len=125`.

**Severity:** High

**Regression test:** `pbt-native/inode_write_prealloc_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B pbt-native/build -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build --target inode_write_prealloc_baseline_pbt_test -j$(nproc)
ctest --test-dir pbt-native/build -R '^inode_write_prealloc_baseline_pbt_test$' --output-on-failure
```

## Root Cause

`inode_write` computes `unsigned max_write = MAX_FILE_SIZE - offset` before validating that `offset < MAX_FILE_SIZE`. Because `max_write` is unsigned, `if (max_write < 0)` can never be true. For `offset > MAX_FILE_SIZE`, the subtraction wraps to a large value, so the function treats invalid offsets as valid writes.

## Suggested Fix

Validate the offset before subtracting:

```c
if (offset >= MAX_FILE_SIZE) {
    return 0;
}
unsigned max_write = MAX_FILE_SIZE - offset;
```
