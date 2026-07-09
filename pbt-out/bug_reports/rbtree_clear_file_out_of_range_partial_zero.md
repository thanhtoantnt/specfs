# Bug: rbtree clear_file partially zeroes out-of-range clears

**Law:** A `clear_file` request whose range exceeds `MAX_FILE_SIZE` should be rejected atomically and preserve existing file data.
**Impact:** Callers can silently zero part of the final allocated page even though the requested clear extends past the maximum file size.
**Function:** `eval/rbtree/optimization/lowlevel_file.c:clear_file`
**Detected by:** Algebraic — Invariant (4d)
**Minimal input:** `offset = MAX_FILE_SIZE - 3`, `len = 5`, final page allocated and filled with non-zero bytes (`oob_tail=3`, `oob_extra=2`).
**Expected:** The final page remains unchanged because `offset + len > MAX_FILE_SIZE` is outside the valid file range.
**Actual:** `clear_file` delegates to `file_write(..., NULL)`, which zeroes the in-range suffix of the final page before stopping at `page >= INDEXTB_NUM`.
**Severity:** medium
**Regression test:** `pbt-native/rbtree_clear_file_pbt_test.c` property `out_of_range_is_rejected`

## Evidence

- `eval/rbtree/optimization/lowlevel_file.c:286` implements `clear_file` as `file_write(node, start, len, NULL)`.
- `eval/rbtree/optimization/lowlevel_file.c:252` calls `file_allocate`, whose out-of-range rejection does not stop the later write loop.
- `eval/rbtree/optimization/lowlevel_file.c:263` stops only after reaching the first page beyond `INDEXTB_NUM`.
- `eval/rbtree/optimization/lowlevel_file.c:272` zeroes the chunk in the final valid page before that stop condition.

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build
cmake --build /tmp/specfs-pbt-build --target rbtree_clear_file_pbt_test
cd /tmp/specfs-pbt-build && ctest -R '^rbtree_clear_file_pbt_test$' --output-on-failure
```

Observed result: `zeros_target_span`, `allocates_sparse_range`, and `zero_len_is_noop` pass; `out_of_range_is_rejected` fails on all generated out-of-range trials.

## Suggested fix

Add an overflow-safe bounds guard to `file_write` before allocation or mutation, for example reject when `node == NULL`, `len == 0`, `offset > MAX_FILE_SIZE`, or `len > MAX_FILE_SIZE - offset`. Because `clear_file` delegates to `file_write`, this fixes both APIs.
