# encrypt file_write rejects NULL data — clear_file is a silent no-op

## Status
Confirmed — failing theft property `null_data_zero_fills` (100/100 fail) against
the REAL compiled `eval/loc/gen/encrypt/lowlevel_file.c::file_write`.

## Property
`encrypt_lowlevel_file_write_pbt_test.null_data_zero_fills` (spec-contract oracle).

The spec's `file_clear.c` calls `file_write(node, start, len, NULL)` to zero-fill
a region. The contract: passing `NULL` as `data` must fill `[start, start+len)`
with zeroes, not be a no-op.

## Witness
```
Pre-fill page with 0x5A.
file_write(&tb, offset, len, NULL);   // should zero-fill
file_read(&tb, offset, len, readback);
// readback[0] = 0x5A (unchanged — NOT zeroed)
```

100/100 trials fail — every NULL-data write leaves the existing data untouched.

## Root Cause
`eval/loc/gen/encrypt/lowlevel_file.c:87`:

```c
void file_write(struct indextb *tb, unsigned offset, unsigned len, const char *data) {
    if (len == 0 || data == NULL || tb == NULL || tb->parent_inode == NULL) {
        return;   // ← BUG: rejects NULL data instead of zero-filling
    }
```

The guard treats `data == NULL` as invalid input and returns. But the spec's
`file_clear.c` explicitly passes NULL:

```c
void file_clear(struct inode *node, unsigned start, unsigned len) {
    file_write(node, start, len, NULL);  // NULL triggers zero-fill
}
```

## Effect
- `clear_file` (which calls `file_write(..., NULL)`) is a **silent no-op** in the
  encrypt variant — the target region is NOT zeroed.
- `inode_truncate`'s shrink path calls `clear_file` → silently fails to clear.
- `inode_write`'s growth path calls `clear_file` to zero new pages → silently fails.
- Any cleared region retains stale encrypted/plaintext data — data leakage.

## Severity
**Medium** — silent data corruption (stale data not cleared). The checksum variant
(`eval/loc/gen/checksum/lowlevel_file.c`) is worse: it `assert(data != NULL)` which
aborts the process.

## Fix
Handle NULL data as a zero-fill request, not an error:

```c
void file_write(struct indextb *tb, unsigned offset, unsigned len, const char *data) {
    if (len == 0 || tb == NULL || tb->parent_inode == NULL) {
        return;
    }
    // If data is NULL, use a zero buffer for the write path
    static const unsigned char zeros[PG_SIZE] = {0};
    const char *write_data = data ? data : (const char *)zeros;
    // ... rest of the function using write_data
```

Or restructure to handle the NULL case separately (zero-fill each page).

## Reproduction
```bash
cd ~/evaluation/specfs
cmake --build pbt-native/build --target encrypt_lowlevel_file_write_pbt_test -j$(nproc)
./pbt-native/build/encrypt_lowlevel_file_write_pbt_test
# null_data_zero_fills: 100/100 fail (byte 0 = 0x5A, expected 0x00)
```
