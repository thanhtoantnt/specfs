# inode_truncate skips clearing newly exposed bytes in pre_alloc variant

## Target
- `eval/pre_alloc/optimization/inode_management.c`
- Function: `inode_truncate`

## Summary
When truncating an inode to a larger size, `inode_truncate` updates `node->size` but does not call `clear_file` for the newly exposed byte range. This leaves the growth region uncleared/uninitialized according to the stated truncate contract in the generated file prompt, which says `inode_truncate` should update size and simulate block effects via `clear_file`.

## Property Failure
The property `grow_clears_new_region` generates `old_size < new_size`, calls `inode_truncate(&node, new_size)`, and expects exactly one clear operation for `[old_size, new_size - old_size)`.

Observed counterexample from the PBT run:

```text
bad grow clear: old=402949 new=662124 calls=0 start=0 len=0
Counter-Example: grow_clears_new_region
Argument 0: {old_size=402949, new_size=662124}
```

## Expected Behavior
For `old_size < new_size`, `inode_truncate` should clear the newly exposed interval:

```c
clear_file(node, old_size, size - old_size);
node->size = size;
```

## Actual Behavior
For `size >= old_size`, the implementation only assigns `node->size = size` and returns, so `clear_file` is never called for growth.

## Reproduction
```sh
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target inode_truncate_prealloc_pbt_test
./pbt-native/build/inode_truncate_prealloc_pbt_test
```

## Added Test
- `pbt-native/inode_truncate_prealloc_pbt_test.c`
