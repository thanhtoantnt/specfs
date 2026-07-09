# Bug: pre_alloc inode_read returns non-NULL buffer for zero-length read

## Status
Confirmed — failing theft property `empty_reads_return_null_buffer` against the REAL compiled `eval/pre_alloc/optimization/inode_management.c::inode_read`.

## Property
`inode_read_prealloc_pbt_test.empty_reads_return_null_buffer` (contract oracle).

When `len == 0` and `offset < node->size`, `inode_read` should return a `read_ret` with `num == 0` and `buf == NULL` (no data means no buffer). The implementation returns `num == 0` but `buf` is a non-NULL pointer from `malloc_buffer(0)`.

## Witness
```text
{size=2898, offset=2650, len=0} -> num=0 buf=0x56412c23a3e0 (non-NULL, expect NULL)
```

The run found 74 failures in 500 trials; every zero-length read hit the same contract violation.

## Root Cause
`eval/pre_alloc/optimization/inode_management.c:145`:

```c
if (offset >= file_size) {
    struct read_ret* ret = malloc_readret();
    if (ret == NULL) {
        return NULL;
    }
    ret->num = 0;
    ret->buf = NULL;
    return ret;
}
unsigned actual_len = len;
if (offset + len > file_size) {
    actual_len = file_size - offset;
}
char* buf = malloc_buffer(actual_len);  // actual_len can be 0
```

When `len == 0` but `offset < file_size`, the code allocates a zero-length buffer instead of returning `NULL`.

## Severity
Low — memory-management contract violation. It does not corrupt data, but it returns a non-NULL buffer where the spec says there is no data to return.

## Fix
Return a NULL buffer for zero-length reads:

```c
char *buf = actual_len > 0 ? malloc_buffer(actual_len) : NULL;
```

## Reproduction
```bash
cmake --build pbt-native/build --target inode_read_prealloc_pbt_test -j$(nproc)
./pbt-native/build/inode_read_prealloc_pbt_test
# empty_reads_return_null_buffer: FAIL
```
