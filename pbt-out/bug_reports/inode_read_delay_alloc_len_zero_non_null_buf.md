# Bug: delay_alloc inode_read returns non-NULL buffer for zero-length read

## Status
Confirmed — failing theft property `zero_length_reads_return_null_buffer` against the real compiled `eval/delay_alloc/optimization/inode_management.c::inode_read`.

## Property
`inode_read_delay_alloc_pbt_test.zero_length_reads_return_null_buffer` (contract oracle).

For all non-NULL inodes with `offset < node->size`, `inode_read(node, 0, offset)` should return a `read_ret` with `num == 0` and `buf == NULL`. A zero-length read returns no bytes, so callers should not receive an allocated data buffer.

## Witness
```text
{size=1327, offset=814, len=0} -> num=0 buf=0x55d9fbe101a0 (non-NULL, expected NULL)
```

The run found 200 failures in 200 zero-length trials.

## Root Cause
`eval/delay_alloc/optimization/inode_management.c:142` computes `actual_len = min(len, node->size - offset)`, which is `0` for `len == 0`. `eval/delay_alloc/optimization/inode_management.c:143` then calls `malloc_buffer(actual_len)` unconditionally, returning a non-NULL zero-length buffer before setting `ret->num = 0`.

## Fix
Return an empty read result before allocating or calling `file_read` when `actual_len == 0`:

```c
if (actual_len == 0) {
    ret->buf = NULL;
    ret->num = 0;
    return ret;
}
```

## Reproduction
```bash
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target inode_read_delay_alloc_pbt_test -j$(nproc)
./pbt-native/build/inode_read_delay_alloc_pbt_test
# zero_length_reads_return_null_buffer: FAIL
```
