# inode_read returns non-NULL buffer for zero-length read

## Status
Confirmed — failing theft property `len_zero_returns_null_buf` (200/200 fail) against
the REAL compiled `eval/extent/optimization/inode_management.c::inode_read`.

## Property
`inode_read_pbt_test.len_zero_returns_null_buf` (contract oracle).

When `len == 0` and `offset < node->size`, `inode_read` should return a `read_ret`
with `num == 0` and `buf == NULL` (no data → no buffer). The implementation returns
`num == 0` but `buf` is a non-NULL pointer (from `malloc_buffer(0)` → `malloc(0)`).

## Witness
```
{size=8578, offset=7168, len=0} → num=0 buf=0x563ad64b6fa0 (non-NULL, expect NULL)
```

200/200 trials fail — every zero-length read returns a dangling buffer pointer.

## Root Cause
`eval/extent/optimization/inode_management.c:140`:

```c
unsigned actual_len = len;  // 0
if (offset + len > file_size) {
    actual_len = file_size - offset;
}
char* buf = malloc_buffer(actual_len);  // malloc(0) → non-NULL on most libc
...
ret->buf = buf;  // non-NULL even though num == 0
ret->num = actual_len;  // 0
```

`malloc(0)` returns a non-NULL pointer on most implementations. The caller gets a
buffer they must `free` even though it contains 0 bytes — a memory management
contract violation (caller may leak the buffer or double-free).

## Severity
**Low** — memory management contract violation (non-NULL buffer for 0-length read).
No crash or data corruption, but callers may leak the buffer or mishandle it.

## Fix
Return NULL buffer for zero-length reads:

```c
char* buf = actual_len > 0 ? malloc_buffer(actual_len) : NULL;
```

## Reproduction
```bash
cmake --build pbt-native/build --target inode_read_pbt_test -j$(nproc)
./pbt-native/build/inode_read_pbt_test
# len_zero_returns_null_buf: 200/200 fail
```
