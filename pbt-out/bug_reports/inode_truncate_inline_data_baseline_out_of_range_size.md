# inode_truncate inline_data baseline accepts sizes beyond MAX_FILE_SIZE

## Target
- `eval/inline_data/baseline/inode_management.c`
- Function: `inode_truncate`

## Summary
`inode_truncate` accepts `size > MAX_FILE_SIZE`, calls `file_allocate` and `clear_file`, and sets `node->size` to the out-of-range value. The inline-data baseline filesystem defines `MAX_FILE_SIZE` as the maximum representable file size, and the generated prompt for inode operations requires bounds checking.

## Property Failure
The theft property `out_of_range_size_is_rejected` generates `requested_size > MAX_FILE_SIZE`, calls `inode_truncate(&node, requested_size)`, and expects the operation to leave the inode and storage callbacks untouched.

Observed counterexample from the PBT run:

```text
out_of_range_size_is_rejected failed: old=33554432 requested=33554442 size=33554442 allocs=1 clears=1 reads=0 writes=0
Counter-Example: out_of_range_size_is_rejected
Argument 0: {old_size=33554432, requested_size=33554442}
```

## Expected Behavior
For `size > MAX_FILE_SIZE`, `inode_truncate` should reject or no-op without changing `node->size` and without allocating/clearing storage.

## Actual Behavior
The implementation treats the oversized request as a normal grow operation and then assigns the oversized size:

```c
if (size > node->size) {
    file_allocate(node->file, node->size, size - node->size);
    clear_file(node, node->size, size - node->size);
}
node->size = size;
```

## Reproduction
```sh
BUILD_DIR=/tmp/specfs-pbt-inode-truncate-inline-data-baseline
cmake -S pbt-native -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --target inode_truncate_inline_data_baseline_pbt_test
"$BUILD_DIR/inode_truncate_inline_data_baseline_pbt_test"
```

## Added Test
- `pbt-native/inode_truncate_inline_data_baseline_pbt_test.c`
