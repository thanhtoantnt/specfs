# inode_truncate inline_data baseline crashes on NULL inode

## Target
- `eval/inline_data/baseline/inode_management.c`
- Function: `inode_truncate`

## Summary
`inode_truncate(NULL, size)` dereferences `node->size` before checking whether `node` is NULL. The inode operation prompt requires null-pointer checks before use, so a NULL inode should be rejected or treated as a no-op instead of crashing.

## Property Failure
The theft property `null_inode_is_noop` runs `inode_truncate(NULL, requested_size)` in a child process and expects normal exit without storage side effects.

Observed failure from the PBT run:

```text
null_inode_is_noop failed: requested=0 signal=11
Counter-Example: null_inode_is_noop
Argument 0: {old_size=691, requested_size=0}
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
BUILD_DIR=/tmp/specfs-pbt-inode-truncate-inline-data-baseline
cmake -S pbt-native -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --target inode_truncate_inline_data_baseline_pbt_test
"$BUILD_DIR/inode_truncate_inline_data_baseline_pbt_test"
```

## Added Test
- `pbt-native/inode_truncate_inline_data_baseline_pbt_test.c`
