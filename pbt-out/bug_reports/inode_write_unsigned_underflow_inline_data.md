# inline_data inode_write unsigned underflow in clamping path

## Status
Confirmed — failing theft property `return_is_clamped` (and 4 others) against
`eval/inline_data/optimization/inode_management.c::inode_write`.

## Note Correction
The findings note previously listed inline_data as CORRECT for bug #2 (unsigned
underflow). This is WRONG — inline_data has the SAME underflow class but in a
DIFFERENT location (the clamping expression, not the guard).

## Root Cause
```c
unsigned inode_write(..., unsigned len, unsigned offset) {
    unsigned clamped_len = len;
    unsigned new_size = max(node->size, offset + len);
    if (offset + len > MAX_FILE_SIZE) {
        clamped_len = MAX_FILE_SIZE - offset;  // ← UNDERFLOWS when offset > MAX_FILE_SIZE
        new_size = MAX_FILE_SIZE;
    }
    ...
    return clamped_len;  // returns huge underflowed value
}
```

When `offset > MAX_FILE_SIZE`:
- `offset + len > MAX_FILE_SIZE` → TRUE (triggers correctly)
- `clamped_len = MAX_FILE_SIZE - offset` → UNDERFLOWS (e.g., 33554432 - 4294966839 = 33554889 as unsigned)
- Returns the underflowed value instead of 0

The fix: add `if (offset >= MAX_FILE_SIZE) return 0;` BEFORE the clamping.

## Witness
```
{offset=4294966839, len=426} → returned 33554889 (expected 0)
{offset=4294967023, len=231} → returned 33554705 (expected 0)
```

## Severity
**High** — same as the other variants (out-of-bounds file_write with huge clamped_len).
Affects inline_data variant, which was previously thought to be correct.

## Reproduction
```bash
cmake --build pbt-native/build --target inode_write_inline_data_pbt_test
./pbt-native/build/inode_write_inline_data_pbt_test
```
