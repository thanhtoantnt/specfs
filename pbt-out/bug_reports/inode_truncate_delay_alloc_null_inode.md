# inode_truncate delay_alloc crashes on NULL inode

- Target: `eval/delay_alloc/optimization/inode_management.c`
- Function: `inode_truncate`
- Property: `null_inode_is_noop`
- Test: `pbt-native/inode_truncate_delay_alloc_safety_pbt_test.c`

## Summary

`inode_truncate(NULL, size)` dereferences `node->size` before checking whether `node` is NULL. The inode operation prompt requires null-pointer checks before use, so a NULL inode should be rejected or treated as a no-op instead of crashing.

## Property

Formal: For all unsigned requested sizes, `inode_truncate(NULL, size)` exits normally and invokes no low-level storage callbacks.

Oracle: Crash-only plus negative/error contract. The property executes the call in a child process so a segmentation fault is reported as a property failure without terminating the runner.

## Evidence

Command:

```sh
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target inode_truncate_delay_alloc_safety_pbt_test -j2
cd pbt-native/build && ./inode_truncate_delay_alloc_safety_pbt_test
```

Example counterexample:

```text
null inode crashed with signal 11 for requested_size=33554433
```

## Expected Fix Direction

Return immediately when `node == NULL` before reading `node->size`, `node->file`, or calling any low-level storage function.
