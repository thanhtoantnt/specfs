# Bug: `locate` crashes on NULL API inputs

**Law:** `locate(cur, path)` must handle NULL API inputs safely: if `cur == NULL` or `path == NULL`, it should return `NULL` without dereferencing either pointer.

**Impact:** Callers that pass a NULL current inode or NULL path array can crash the filesystem process. This violates the generated-code prompt requirement to "Include proper NULL checks" and makes path resolution unsafe on malformed or failed-allocation inputs.

**Function:** `locate` in `eval/extent/baseline/path_handling.c`

**Detected by:** Negative/error-contract property (`null_inputs_return_null_without_crash`) in `pbt-native/locate_baseline_pbt_test.c`

**Minimal input:** `{null_cur=0, null_path=1, nonempty_path=0}` — a valid `cur` inode and `path == NULL`.

**Expected:** Return `NULL` without crashing.

**Actual:** Segmentation fault. `locate` enters `while (path[i] != NULL)` without checking `path`, and later dereferences `current->dir` without checking `cur` for non-empty paths.

**Severity:** high

**Regression test:** `pbt-native/locate_baseline_pbt_test.c` (`prop_null_inputs_return_null_without_crash`)

**Reproduction command:**

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-locate-baseline -DCMAKE_BUILD_TYPE=Debug \
  && cmake --build /tmp/specfs-pbt-locate-baseline --target locate_baseline_pbt_test -j$(nproc) \
  && ctest --test-dir /tmp/specfs-pbt-locate-baseline -R '^locate_baseline_pbt_test$' --output-on-failure
```
