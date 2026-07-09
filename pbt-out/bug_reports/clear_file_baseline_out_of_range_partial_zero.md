# Bug: baseline extent clear_file partially zeroes out-of-range clears

**Law:** A `clear_file` request whose byte range exceeds `MAX_FILE_SIZE` must be rejected atomically or treated as a no-op; it must not partially mutate the in-range suffix of the final page.

**Impact:** Callers that pass an invalid clear range can silently lose valid data near EOF. Because `clear_file` implements zero-fill/truncation behavior, a failed oversized clear can corrupt valid bytes before the out-of-range part is reached.

**Function:** `eval/extent/baseline/lowlevel_file.c:clear_file`

**Detected by:** Theft property `out_of_range_does_not_crash_or_mutate` in `pbt-native/clear_file_baseline_pbt_test.c` (Algebraic — Invariant / boundary negative contract).

**Minimal input:** Allocate/fill the final page (`INDEXTB_NUM - 1`) with a non-zero byte, then call:

```c
clear_file(&node, MAX_FILE_SIZE - 1U, 2U);
```

**Expected:** The request exceeds `MAX_FILE_SIZE`, so existing bytes in the final valid page remain unchanged and the operation does not write past the index table.

**Actual:** `clear_file` delegates directly to `file_write(node->file, start, len, NULL)`. `file_write` zeroes the valid suffix of the final page first, then advances to page `INDEXTB_NUM` without a bounds guard. In the observed PBT run the child process exited with code 2, meaning the final page was mutated.

**Severity:** medium

**Regression test:** `pbt-native/clear_file_baseline_pbt_test.c` property `out_of_range_does_not_crash_or_mutate`.

## Evidence

- `eval/extent/baseline/lowlevel_file.c:150-188` iterates pages in `file_write` without checking `page < INDEXTB_NUM`.
- `eval/extent/baseline/lowlevel_file.c:171-172` zeroes existing page bytes when `data == NULL`.
- `eval/extent/baseline/lowlevel_file.c:190-191` implements `clear_file` as an unchecked delegate to `file_write`.

Reproducing command:

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-clear-baseline-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-clear-baseline-build --target clear_file_baseline_pbt_test -j2
cd /tmp/specfs-pbt-clear-baseline-build && ctest -R '^clear_file_baseline_pbt_test$' --output-on-failure
```

Observed result: 3 properties pass; `out_of_range_does_not_crash_or_mutate` fails on generated cases such as `{oob_tail=1, oob_extra=51}` with `exit=2` (final page mutated).
