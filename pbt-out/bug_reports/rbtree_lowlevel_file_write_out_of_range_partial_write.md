# rbtree file_write partially applies out-of-range writes

## Target
- `eval/rbtree/optimization/lowlevel_file.c:file_write`

## Property
A write whose requested range exceeds `MAX_FILE_SIZE` should be rejected without mutating existing file data. This matches the adjacent allocation guard in `file_allocate`, which returns when `offset + len > MAX_FILE_SIZE`.

## Finding
`file_write` calls `file_allocate(node, offset, len)`, but then proceeds with the copy loop even if allocation rejected the range. For an already allocated last page, an out-of-range write beginning before `MAX_FILE_SIZE` mutates the in-range prefix and then stops only when `page >= INDEXTB_NUM`.

Relevant code:
- `eval/rbtree/optimization/lowlevel_file.c:251` enters `file_write`.
- `eval/rbtree/optimization/lowlevel_file.c:252` calls `file_allocate`, which rejects out-of-range allocation.
- `eval/rbtree/optimization/lowlevel_file.c:263` breaks only after copying any chunks before the first out-of-bounds page.
- `eval/rbtree/optimization/lowlevel_file.c:269` performs the mutation.

## Reproduction
The property `out_of_range_write_is_rejected` in `pbt-native/rbtree_lowlevel_file_write_pbt_test.c` creates an allocated final page, snapshots it, then generates writes where `offset + len > MAX_FILE_SIZE`. All generated trials mutated the final page.

Command:
```sh
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target rbtree_lowlevel_file_write_pbt_test
cd pbt-native/build && ctest -R '^rbtree_lowlevel_file_write_pbt_test$' --output-on-failure
```

Observed result:
- `roundtrip_matches_input`: PASS
- `write_preserves_unrelated_bytes`: PASS
- `null_data_zero_fills`: PASS
- `out_of_range_write_is_rejected`: FAIL, 100/100 generated out-of-range trials mutated data

## Impact
Callers can observe partial writes for requests that exceed the maximum file size, despite allocation rejecting the same range. This violates an atomic reject-style bounds contract and can silently corrupt the last allocated page.

## Suggested fix
Return early in `file_write` when `len == 0`, `node == NULL`, or `offset + len > MAX_FILE_SIZE`, using overflow-safe arithmetic such as `len > MAX_FILE_SIZE - offset`.
