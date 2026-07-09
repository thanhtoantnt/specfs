# inode_truncate inline_data crashes on NULL inode

## Target
- `eval/inline_data/optimization/inode_management.c`
- Function: `inode_truncate`

## Summary
`inode_truncate(NULL, size)` dereferences `node->size` before checking whether `node` is NULL. The inode operation prompt requires null-pointer checks before use, so a NULL inode should be rejected or treated as a no-op instead of crashing.

## Property Failure
The theft property `null_inode_is_noop` runs `inode_truncate(NULL, new_size)` in a child process and expects normal exit without storage side effects.

Observed failure from the PBT run:

```text
null inode crashed with signal 11
Counter-Example: null_inode_is_noop
Argument 0: {old_size=25174106, new_size=15664942}
```

## Expected Behavior
`inode_truncate` should check for a NULL inode before dereferencing it:

```c
if (node == NULL) return;
```

## Actual Behavior
The implementation immediately evaluates `size > node->size`, causing a segmentation fault when `node == NULL`.

## Reproduction
```sh
cmake -S pbt-native -B pbt-native/build -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build --target inode_truncate_inline_data_pbt_test
cd pbt-native/build && ctest -R '^inode_truncate_inline_data_pbt_test$' --output-on-failure
```

## Added Test
- `pbt-native/inode_truncate_inline_data_pbt_test.c`
