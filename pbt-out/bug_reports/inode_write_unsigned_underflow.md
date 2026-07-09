# inode_write unsigned underflow allows writes past MAX_FILE_SIZE

## Status
Confirmed — failing theft property `return_is_clamped` with shrunk witnesses against
the REAL compiled `eval/extent/optimization/inode_management.c::inode_write`.

## Property
`inode_write_pbt_test.return_is_clamped` (clamping invariant).

When `offset >= MAX_FILE_SIZE`, `inode_write` must return 0 (no bytes written) — the
write would exceed the file's maximum page-table capacity. The spec says:
> `inode_write` returns clamped `len` if exceeding `MAX_FILE_SIZE`.

## Shrunk Witnesses
```
{initial_size=6926042,  offset=4294967068, len=186}  → inode_write returns 186 (expect 0)
{initial_size=29366458, offset=33554796,   len=210}  → inode_write returns 210 (expect 0)
{initial_size=15241648, offset=4294967059, len=346}  → inode_write returns 346 (expect 0)
{initial_size=6926042,  offset=4294967068, len=206}  → inode_write returns 206 (expect 0)
```

Note `offset=4294967068` is `0xFFFFFF7C` — well above `MAX_FILE_SIZE` (`0x2000000` = 33,554,432).

## Root Cause
`eval/extent/optimization/inode_management.c:173`:

```c
unsigned max_write = MAX_FILE_SIZE - offset;
if (max_write < 0) {    // ← ALWAYS FALSE: unsigned can never be negative
    return 0;
}
unsigned actual_len = len;
if (len > max_write) {
    actual_len = max_write;   // max_write is ~4 billion due to underflow
}
```

`max_write` is `unsigned`, so the `< 0` guard is dead code. When `offset > MAX_FILE_SIZE`,
the subtraction wraps to a very large positive number (~4 billion). The `len > max_write`
check passes (len is never that large), so `actual_len` stays as `len`. The function
returns `len` instead of clamping to 0, allowing writes far beyond the file's page table.

The baseline uses `offset + len > MAX_FILE_SIZE` (no underflow).

## Effect
- Writes with `offset >= MAX_FILE_SIZE` return a non-zero byte count instead of 0.
- The subsequent `file_write(node, offset, actual_len, buffer)` call passes an
  out-of-range offset and length, which can corrupt the extent tree or crash.
- Callers that trust the return value as "bytes successfully written" get wrong
  results.

## Severity
**High** — out-of-bounds write via unsigned underflow. Triggered by any
`offset >= MAX_FILE_SIZE` (33,554,432). The Coq spec explicitly requires clamping,
and the baseline implementation handles it correctly.

## Fix
Check for overflow before the subtraction:

```c
if (offset >= MAX_FILE_SIZE) {
    return 0;
}
unsigned max_write = MAX_FILE_SIZE - offset;
```

## Reproduction
```bash
cd ~/evaluation/specfs
cmake -B pbt-native/build -S pbt-native -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build -j$(nproc)
(cd pbt-native/build && ctest -R inode_write_pbt_test --output-on-failure)
# return_is_clamped fails: "got 186 expected 0" for offset >= MAX_FILE_SIZE
```
