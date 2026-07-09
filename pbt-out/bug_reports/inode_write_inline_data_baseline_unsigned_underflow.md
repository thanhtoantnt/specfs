# Bug: inline_data baseline inode_write accepts out-of-range offsets

**Law:** `inode_write(node, buffer, len, offset)` must clamp writes to `MAX_FILE_SIZE`; when `offset >= MAX_FILE_SIZE`, the effective written length is `0`, no low-level write/allocation/clear should occur, and `node->size` must remain unchanged.

**Function:** `inode_write` in `eval/inline_data/baseline/inode_management.c`

**Detected by:** Negative/Error Contract and Algebraic Invariant properties in `pbt-native/inode_write_inline_data_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build
cmake --build /tmp/specfs-pbt-build --target inode_write_inline_data_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-build/inode_write_inline_data_baseline_pbt_test
```

Observed run summary:

```text
== FAIL 'return_is_clamped': pass 408, fail 92, skip 0, dup 0
== FAIL 'out_of_range_offsets_are_noops': pass 324, fail 176, skip 0, dup 0
== PASS 'size_never_exceeds_max_file_size': pass 500, fail 0, skip 0, dup 0
== FAIL 'growth_is_allocated_and_cleared': pass 367, fail 133, skip 0, dup 0
== FAIL 'file_write_matches_effective_write': pass 345, fail 155, skip 0, dup 0
4 failure(s)
```

## Root Cause

`inode_write` computes `offset + len` in unsigned arithmetic and then uses `MAX_FILE_SIZE - offset` without first proving that `offset < MAX_FILE_SIZE`:

```c
unsigned new_size = max(node->size, offset + len);
if (offset + len > MAX_FILE_SIZE) {
    clamped_len = MAX_FILE_SIZE - offset;
    new_size = MAX_FILE_SIZE;
}
```

For `offset > MAX_FILE_SIZE`, `MAX_FILE_SIZE - offset` wraps to a large unsigned value. For `offset == MAX_FILE_SIZE`, `clamped_len` becomes `0`, but the function still allocates/clears growth to `MAX_FILE_SIZE` and calls `file_write` with a zero-length range.

## Expected Fix

Validate `offset >= MAX_FILE_SIZE` before doing unsigned addition/subtraction. Return `0` and leave the inode and low-level file state untouched for out-of-range offsets; otherwise compute the remaining capacity as `MAX_FILE_SIZE - offset` and clamp `len` against that value.
