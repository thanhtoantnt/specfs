# Bug: inline_data clear_file partially zeroes out-of-range clears

**Law:** A `clear_file` request whose range exceeds `MAX_FILE_SIZE` must be rejected atomically or treated as a no-op; it must not partially mutate the in-range suffix of the final page.

**Impact:** Callers that pass an invalid clear range can silently lose valid data near EOF. Because `clear_file` is used to implement zero-fill/truncation semantics, partial mutation can corrupt existing file contents while still leaving the requested operation incomplete.

**Function:** `eval/inline_data/optimization/lowlevel_file.c:clear_file`

**Detected by:** Theft property `out_of_range_clear_is_noop` in `pbt-native/inline_data_clear_file_pbt_test.c` (Algebraic — Invariant / boundary contract).

**Observed failing input:**

```text
{oob_tail=36, oob_extra=44, fill=134}
```

This maps the checked boundary case to:

```c
offset = MAX_FILE_SIZE - 36;
len = 36 + 44;
```

with the final page already allocated and filled with non-zero bytes.

**Expected:** The final page remains unchanged because `offset + len > MAX_FILE_SIZE` is outside the valid file byte range.

**Actual:** `clear_file` delegates to `file_write(node, start, len, NULL)`. `file_write` calls `file_allocate`, whose bounds check rejects the allocation, but then the write loop still runs and zeroes the in-range suffix of the allocated final page before stopping at `page >= INDEXTB_NUM`.

**Severity:** High — invalid clear requests can silently corrupt valid in-range data.

**Regression test:** `pbt-native/inline_data_clear_file_pbt_test.c` property `out_of_range_clear_is_noop`.

## Reproduction

```bash
cmake -S pbt-native -B /tmp/specfs-pbt-inline-clear
cmake --build /tmp/specfs-pbt-inline-clear --target inline_data_clear_file_pbt_test -j2
cd /tmp/specfs-pbt-inline-clear && ctest --output-on-failure -R '^inline_data_clear_file_pbt_test$'
```

Observed result: `clear_zeros_target_span_and_preserves_rest`, `sparse_clear_materializes_zero_bytes`, and `zero_length_clear_is_noop` pass; `out_of_range_clear_is_noop` fails on generated out-of-range clear ranges.

## Suggested fix

Add an overflow-safe bounds guard before any mutation in `file_write` (and/or in `clear_file` before delegation), e.g. reject/no-op when `node == NULL`, `len == 0`, `offset > MAX_FILE_SIZE`, or `len > MAX_FILE_SIZE - offset`. Since `clear_file` delegates to `file_write`, fixing `file_write` fixes both APIs.
