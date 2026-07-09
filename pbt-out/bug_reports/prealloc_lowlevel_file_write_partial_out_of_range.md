# pre_alloc file_write partially applies out-of-range writes

## Status
Confirmed — failing theft property `partial_out_of_range_is_noop` (200/200 fail) against
the REAL compiled `eval/pre_alloc/optimization/lowlevel_file.c::file_write`.

## Property
A write whose range extends past `MAX_FILE_SIZE` should be rejected completely (no mutation).
Instead, `file_write` partially writes the in-range prefix before breaking at `page >= INDEXTB_NUM`.

## Root Cause
Same as rbtree variant:
- `file_write` calls `file_allocate(node, offset, len)` which rejects out-of-range allocations
- BUT the copy loop proceeds regardless, writing data to pages that already exist
- Only stops when `page >= INDEXTB_NUM` — but by then, the last allocated page has been mutated

```c
void file_write(struct inode *node, unsigned offset, unsigned len, const char *data) {
    file_allocate(node, offset, len);  // rejects if offset+len > MAX_FILE_SIZE
    // ... BUT copy loop proceeds anyway:
    while (remaining > 0) {
        unsigned page = cur / PG_SIZE;
        if (page >= INDEXTB_NUM) break;  // too late — already wrote to last page
        Extent *ext = find_extent(node, page);
        if (ext) {
            memcpy(ext->data + ext_offset, src, copy_len);  // partial write!
        }
        ...
    }
}
```

## Reproduction
```bash
cmake --build pbt-native/build --target prealloc_lowlevel_file_write_pbt_test
./pbt-native/build/prealloc_lowlevel_file_write_pbt_test
# partial out-of-range is noop: 200/200 fail
```
