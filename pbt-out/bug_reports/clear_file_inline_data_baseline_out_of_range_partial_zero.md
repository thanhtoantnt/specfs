# Bug: inline_data baseline clear_file partially zeroes out-of-range clears

**Law:** A `clear_file` request whose byte range exceeds `MAX_FILE_SIZE` must be rejected atomically or treated as a no-op; it must not partially mutate the in-range suffix of the final page.

**Impact:** Callers that pass an invalid clear range can silently lose valid data near EOF. Because `clear_file` implements zero-fill/truncation behavior, a failed oversized clear can corrupt valid bytes before the out-of-range part is reached.

**Function:** `eval/inline_data/baseline/lowlevel_file.c:clear_file`

**Detected by:** Theft property `out_of_range_clear_is_noop` in `pbt-native/clear_file_inline_data_baseline_pbt_test.c` (Algebraic — Invariant / boundary negative contract).

**Minimal input:** Allocate/fill the final page (`INDEXTB_NUM - 1`) with a non-zero byte, then call:

```c
clear_file(&node, MAX_FILE_SIZE - 1U, 2U);
```

**Expected:** The request exceeds `MAX_FILE_SIZE`, so existing bytes in the final valid page remain unchanged and the operation does not write past the index table.

**Actual:** `clear_file` delegates directly to `file_write(node->file, start, len, NULL)`. `file_write` zeroes the valid suffix of the final page first, then advances beyond page `INDEXTB_NUM` without an overflow-safe bounds guard. In the observed PBT run the child process exited with code 2, meaning the final page was mutated.

**Severity:** medium

**Regression test:** `pbt-native/clear_file_inline_data_baseline_pbt_test.c` property `out_of_range_clear_is_noop`.

## Evidence

- `eval/inline_data/baseline/lowlevel_file.c:150` iterates `file_write` while bytes remain without checking `page < INDEXTB_NUM`.
- `eval/inline_data/baseline/lowlevel_file.c:158` computes `page = current_offset / PG_SIZE`, which can become `INDEXTB_NUM` for oversized ranges.
- `eval/inline_data/baseline/lowlevel_file.c:169` zeroes existing page bytes when `data == NULL`.
- `eval/inline_data/baseline/lowlevel_file.c:192` implements `clear_file` as an unchecked delegate to `file_write`.

Reproducing command:

```sh
BUILD_DIR=/tmp/pbt-native-specfs
rm -rf "$BUILD_DIR"
cmake pbt-native -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target clear_file_inline_data_baseline_pbt_test -j$(nproc)
cd "$BUILD_DIR" && ctest --output-on-failure -R '^clear_file_inline_data_baseline_pbt_test$'
```

Observed result: 3 properties pass; `out_of_range_clear_is_noop` fails on generated cases such as `{oob_tail=2, oob_extra=3}` with `exit=2` (final page mutated).
