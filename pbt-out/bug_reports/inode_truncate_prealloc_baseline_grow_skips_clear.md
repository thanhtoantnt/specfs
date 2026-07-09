# Bug: pre_alloc baseline inode_truncate skips clearing newly grown range

**Law:** For valid sizes `old_size < requested_size <= MAX_FILE_SIZE`, `inode_truncate(node, requested_size)` must clear exactly the newly exposed byte interval `[old_size, requested_size)` and then set `node->size == requested_size`.

**Impact:** Growing a file can expose stale or uninitialized data on subsequent reads, violating truncate-zero-fill semantics and the generated prompt's requirement to simulate truncate block effects via `clear_file`.

**Function:** `inode_truncate` in `eval/pre_alloc/baseline/inode_management.c`

**Detected by:** Algebraic invariant property `grow_clears_new_region` in `pbt-native/inode_truncate_prealloc_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `{old_size=2, requested_size=7}`.

**Expected:** Call `clear_file(node, old_size, requested_size - old_size)` once and set `node->size = requested_size`.

**Actual:** Sets `node->size = requested_size` and returns without calling `clear_file`.

**Severity:** High

**Regression test:** `pbt-native/inode_truncate_prealloc_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B pbt-native/build -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build --target inode_truncate_prealloc_baseline_pbt_test -j$(nproc)
./pbt-native/build/inode_truncate_prealloc_baseline_pbt_test
```

## Root Cause

The implementation returns immediately for `size >= old_size`:

```c
if (size >= old_size) {
    node->size = size;
    return;
}
```

That branch treats growth as a size-only update and skips clearing the newly exposed file range.
