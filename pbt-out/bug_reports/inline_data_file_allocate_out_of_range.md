# Bug: inline_data `file_allocate` allocates beyond `MAX_FILE_SIZE`

## Target
- Source: `eval/inline_data/optimization/lowlevel_file.c`
- Function: `file_allocate`
- Test: `pbt-native/inline_data_file_allocate_pbt_test.c`
- Target: `inline_data_file_allocate_pbt_test`

## Summary
`file_allocate` for the inline-data variant accepts ranges at or beyond the public `MAX_FILE_SIZE` boundary and materializes page-table entries instead of treating the request as out of range. The inode layer clamps writes and truncates against `MAX_FILE_SIZE`, so low-level allocation should not create storage for bytes outside that logical file limit.

## Property Failure
The theft property `empty_or_out_of_range_allocation_is_noop` generates zero-length or out-of-range allocation requests and expects the page table to remain unchanged.

Observed minimal boundary witness from the PBT run:

```text
Counter-Example: empty_or_out_of_range_allocation_is_noop
Argument 0:
{offset=33554432, len=1}
```

A second crossing-boundary witness was also generated:

```text
{offset=33554431, len=2}
```

## Expected Behavior
For `offset >= MAX_FILE_SIZE` or `offset + len > MAX_FILE_SIZE`, `file_allocate` should reject or no-op without allocating any `indextb` page.

## Actual Behavior
`file_allocate` subtracts `INLINE_DATA_SIZE` before indexing into the page table, so requests at the logical file boundary still map to the final page-table slot:

```text
offset=33554432, len=1 -> page 8191 allocated
```

## Evidence
Command run:

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build >/tmp/specfs-pbt-cmake.log && cmake --build /tmp/specfs-pbt-build --target inline_data_file_allocate_pbt_test -j2 && /tmp/specfs-pbt-build/inline_data_file_allocate_pbt_test
```

Relevant output:

```text
== PROP 'empty_or_out_of_range_allocation_is_noop': 200 trials
 -- Counter-Example: empty_or_out_of_range_allocation_is_noop
    Argument 0:
{offset=33554432, len=1}
F
== FAIL 'empty_or_out_of_range_allocation_is_noop': pass 72, fail 32, skip 0, dup 96
```

## Passing Properties
The same target confirms the valid in-range behavior:

- `valid_range_allocates_expected_pages_zeroed`: valid ranges allocate every intersecting post-inline page and newly allocated bytes are zero-filled.
- `reallocate_is_idempotent_and_preserves_bytes`: reallocating an already allocated range does not add pages or overwrite data.
- `inline_only_allocation_is_noop`: ranges wholly inside `inline_data` do not allocate page-table entries.

## Suggested Fix
Add an early range guard before translating to post-inline page-table offsets, using subtraction to avoid unsigned overflow:

```c
if (inode == NULL || inode->file == NULL || len == 0 || offset >= MAX_FILE_SIZE || len > MAX_FILE_SIZE - offset) return;
```

If the intended inline-data design is to permit `MAX_FILE_SIZE + INLINE_DATA_SIZE` bytes, update the inode-layer bounds and public contract consistently instead.
