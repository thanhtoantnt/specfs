# inode_truncate delay_alloc accepts sizes beyond MAX_FILE_SIZE

- Target: `eval/delay_alloc/optimization/inode_management.c`
- Function: `inode_truncate`
- Property: `out_of_range_size_is_rejected`
- Test: `pbt-native/inode_truncate_delay_alloc_safety_pbt_test.c`

## Summary

`inode_truncate` accepts `size > MAX_FILE_SIZE`, calls `file_allocate` and `clear_file`, and sets `node->size` to the out-of-range value. The inode operation prompt requires bounds checking, and delay_alloc defines `MAX_FILE_SIZE` as the filesystem file-size limit.

## Property

Formal: For all inodes with `old_size <= MAX_FILE_SIZE` and all requested sizes `size > MAX_FILE_SIZE`, `inode_truncate(&node, size)` should reject or no-op: `node.size == old_size` and no low-level storage callbacks are invoked.

Oracle: Negative/error contract with callback observation.

## Evidence

Command:

```sh
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target inode_truncate_delay_alloc_safety_pbt_test -j2
cd pbt-native/build && ./inode_truncate_delay_alloc_safety_pbt_test
```

Example counterexample:

```text
{old_size=0, requested_size=33554433}
out-of-range truncate mutated state: old=0 requested=33554433 size=33554433 alloc=1 clear=1 write=0
```

## Expected Fix Direction

Check `node != NULL` and `size <= MAX_FILE_SIZE` before dereferencing or mutating the inode. For out-of-range sizes, return without changing `node->size` or touching storage.
