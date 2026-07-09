# inode_truncate inline_data accepts sizes beyond MAX_FILE_SIZE

## Target
- `eval/inline_data/optimization/inode_management.c`
- Function: `inode_truncate`

## Summary
`inode_truncate` accepts `size > MAX_FILE_SIZE`, calls `file_allocate` and `clear_file`, and sets `node->size` to the out-of-range value. The inline-data filesystem defines `MAX_FILE_SIZE` as the maximum representable file size, and the generated prompt for inode operations requires bounds checking.

## Property Failure
The theft property `out_of_range_size_is_rejected` generates `new_size > MAX_FILE_SIZE`, calls `inode_truncate(&node, new_size)`, and expects the operation to leave the inode and storage callbacks untouched.

Observed counterexample from the PBT run:

```text
out-of-range truncate mutated state: old=6904777 new=33554699 size=33554699 alloc=1 clear=1
Counter-Example: out_of_range_size_is_rejected
Argument 0: {old_size=6904777, new_size=33554699}
```

## Expected Behavior
For `size > MAX_FILE_SIZE`, `inode_truncate` should reject or no-op without changing `node->size` and without allocating/clearing storage.

## Actual Behavior
The implementation treats the oversized request as a normal grow operation:

```c
if (size > node->size) {
    file_allocate(node, node->size, size - node->size);
    clear_file(node, node->size, size - node->size);
}
node->size = size;
```

## Reproduction
```sh
cmake -S pbt-native -B pbt-native/build -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build --target inode_truncate_inline_data_pbt_test
cd pbt-native/build && ctest -R '^inode_truncate_inline_data_pbt_test$' --output-on-failure
```

## Added Test
- `pbt-native/inode_truncate_inline_data_pbt_test.c`
