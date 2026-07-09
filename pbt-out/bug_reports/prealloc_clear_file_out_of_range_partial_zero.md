# Bug: pre_alloc clear_file partially zeroes out-of-range clears

**Law:** A `clear_file` request whose byte range exceeds `MAX_FILE_SIZE` must be rejected atomically or treated as a no-op; it must not partially mutate the in-range suffix of the final page.

**Impact:** Callers that pass an invalid clear range can silently lose valid data near EOF. Because `clear_file` is used to implement zero-fill/truncation semantics, partial mutation can corrupt existing file contents while still leaving the requested operation incomplete.

**Function:** `eval/pre_alloc/optimization/lowlevel_file.c:clear_file`

**Detected by:** Theft property `out_of_range_is_rejected` in `pbt-native/prealloc_clear_file_pbt_test.c` (Algebraic — Invariant / boundary contract).

**Minimal input:** Generated failing example from the verified run:

```text
{start_page=10, page_count=2, page_off=3338, len=761,
 oob_tail=6, oob_extra=54, fill=67}
```

This maps the checked boundary case to:

```c
offset = MAX_FILE_SIZE - 6;
len = 6 + 54;
```

with the final page already allocated and filled with non-zero bytes.

**Expected:** The final page remains unchanged because `offset + len > MAX_FILE_SIZE` is outside the valid file byte range.

**Actual:** `clear_file` delegates to `file_write(node, start, len, NULL)`. `file_write` calls `file_allocate`, whose bounds check rejects the allocation, but then the write loop still runs and zeroes the in-range suffix of the allocated final page before stopping at `page >= INDEXTB_NUM`.

**Severity:** High — invalid clear requests can silently corrupt valid in-range data.

**Regression test:** `pbt-native/prealloc_clear_file_pbt_test.c` property `out_of_range_is_rejected`.

## Reproduction

```bash
cmake -S pbt-native -B .pbt-build-prealloc-clear
cmake --build .pbt-build-prealloc-clear --target prealloc_clear_file_pbt_test -j2
cd .pbt-build-prealloc-clear && ctest --output-on-failure -R '^prealloc_clear_file_pbt_test$'
```

Observed result: `zeros_target_span`, `allocates_sparse_range`, and `zero_len_is_noop` pass; `out_of_range_is_rejected` fails on generated out-of-range clear ranges.

Note: `/tmp` was full in the verification environment, so the successful configure/build used the workspace-local `.pbt-build-prealloc-clear` directory.

## Suggested fix

Add an overflow-safe bounds guard before any mutation in `file_write` (and/or in `clear_file` before delegation), e.g. reject/no-op when `node == NULL`, `len == 0`, `offset > MAX_FILE_SIZE`, or `len > MAX_FILE_SIZE - offset`. Since `clear_file` delegates to `file_write`, fixing `file_write` fixes both APIs.
