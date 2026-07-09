# delay_alloc inode_write unsigned underflow in clamping path

## Status  
Confirmed — 5/5 properties FAIL against eval/delay_alloc/optimization/inode_management.c::inode_write.

## Note Correction
The findings note listed delay_alloc as CORRECT. This is WRONG. delay_alloc has the
SAME clamping-path underflow as inline_data: MAX_FILE_SIZE - offset underflows when
offset > MAX_FILE_SIZE.

## Root Cause
Same as inline_data:
```c
if (offset + len > MAX_FILE_SIZE) {
    clamped_len = MAX_FILE_SIZE - offset;  // UNDERFLOWS when offset > MAX_FILE_SIZE
}
```

## Impact
ALL 5 optimization variants now confirmed to have unsigned underflow in inode_write.
The bug appears in two forms:
- extent/rbtree/pre_alloc: dead `max_write < 0` guard
- inline_data/delay_alloc: underflow in clamping expression

Both result in out-of-bounds file_write calls with huge lengths.
