# Bug: `file_allocate(NULL, valid_range)` crashes

## Target
- Function: `file_allocate`
- Source: `eval/extent/optimization/lowlevel_file.c`
- Test: `pbt-native/file_allocate_extent_pbt_test.c` (also `pbt-native/extent_file_allocate_pbt_test.c`)
- Target: `file_allocate_extent_pbt_test` (also `extent_file_allocate_pbt_test`)

## Property
`∀ offset,len. len > 0 ∧ offset + len <= MAX_FILE_SIZE => file_allocate(NULL, offset, len)` returns safely without dereferencing the null inode pointer.

Oracle: negative/error contract and crash-safety oracle. A low-level API that returns `void` should treat a null inode as a no-op or otherwise fail safely; it must not segfault.

## Finding
`file_allocate` dereferences `node` through `is_allocated(node, current)` without checking `node != NULL`, so valid non-empty allocation requests with a null inode crash with `SIGSEGV`.

Minimal shrunk witness from theft:

```text
{start_page=58, page_off=2396, len=9409, seed=254}
```

This produces:

```text
offset = 58 * PG_SIZE + 2396 = 239964
len = 9409
status = 139
```

## Evidence
Command run:

```bash
cmake -S pbt-native -B /tmp/specfs-pbt-build >/tmp/specfs-pbt-cmake.log && cmake --build /tmp/specfs-pbt-build --target extent_file_allocate_pbt_test -j2 && /tmp/specfs-pbt-build/extent_file_allocate_pbt_test
```

Relevant output:

```text
== PROP 'null_inode_is_safe': 1 trials, seed 0xc70c53b5348f7338
 -- Counter-Example: null_inode_is_safe
    Trial 0, Seed 0xc70c53b5348f7338
    Argument 0:
{start_page=58, page_off=2396, len=9409, seed=254}
F
== FAIL 'null_inode_is_safe': pass 0, fail 1, skip 0, dup 0
  [FAIL] null_inode_is_safe

1 failure(s)
```

## Passing Properties
The same target also confirms the non-null allocation behavior:

- `valid_range_allocates_all_pages_zeroed`: valid ranges allocate every intersecting page and read back as zeroes.
- `reallocate_is_idempotent_and_preserves_bytes`: reallocating an already allocated range does not add pages/extents or overwrite data.
- `empty_or_out_of_range_allocation_is_noop`: zero-length and over-`MAX_FILE_SIZE` allocations do not mutate an empty inode.

## Suggested Fix
Add an early guard in `file_allocate` before any access through `node`:

```c
if (node == NULL || len == 0 || offset + len > MAX_FILE_SIZE) return;
```

Consider also using `len > MAX_FILE_SIZE - offset` to avoid unsigned addition overflow in the range guard.
