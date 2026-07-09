# Bug: baseline file_write drops non-NULL writes to sparse pages

**Law:** For any in-range `offset`, positive `len`, and non-NULL data buffer, `file_write(tb, offset, len, data)` followed by `file_read(tb, offset, len, out)` should return exactly the bytes written, allocating zero-initialized pages as needed.

**Impact:** Writes to a newly-created/sparse baseline file can be silently lost. A caller that writes user data before the target pages already exist will later read zeros instead of the submitted bytes.

**Function:** `eval/extent/baseline/lowlevel_file.c::file_write`

**Detected by:** Algebraic — round-trip property (`sparse_write_roundtrips`) in `pbt-native/file_write_baseline_pbt_test.c`

**Minimal input:** Empty `struct indextb`, `offset = 0`, `len = 1`, `data[0] = 1`.

**Expected:** `file_write` allocates page 0 and stores byte `1`; `file_read` returns `out[0] == 1`.

**Actual:** `file_write` only allocates pages when `data == NULL`. For a non-NULL write into a NULL page it does nothing, so `file_read` returns `out[0] == 0`.

**Severity:** high

**Regression test:** `pbt-native/file_write_baseline_pbt_test.c::prop_sparse_write_roundtrips`

## Evidence

The failing theft property was run with:

```bash
cmake -S pbt-native -B /tmp/specfs-pbt-native -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-native --target file_write_baseline_pbt_test -j$(nproc)
ctest --test-dir /tmp/specfs-pbt-native -R '^file_write_baseline_pbt_test$' --output-on-failure
```

Observed result:

```text
== FAIL 'sparse_write_roundtrips': pass 0, fail 300, skip 0, dup 0
[PASS] allocated_write_preserves_unrelated_bytes
[PASS] null_data_allocates_zero_filled_range
[PASS] zero_length_write_is_noop
1 failure(s)
```

Root cause in `eval/extent/baseline/lowlevel_file.c`: inside the `tb->index[page] == NULL` branch, allocation happens only for `data == NULL`; non-NULL data is not copied and the page remains absent.
