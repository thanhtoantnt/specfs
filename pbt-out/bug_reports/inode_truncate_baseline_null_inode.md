# Bug: baseline extent inode_truncate dereferences NULL inode

**Law:** Public inode operations that accept an inode pointer must check for `NULL` before dereferencing it; `inode_truncate(NULL, size)` should be a safe no-op with no low-level file calls.

**Impact:** A null inode pointer causes an immediate segmentation fault, violating the memory-safety guarantee and allowing callers to crash the filesystem operation path.

**Function:** `inode_truncate` in `eval/extent/baseline/inode_management.c`

**Detected by:** Negative/Error Contract property `null_inode_is_noop` in `pbt-native/inode_truncate_baseline_pbt_test.c`

**Minimal input:** Example generated counterexample: `node=NULL, requested_size=0`.

**Expected:** Return without dereferencing `node` and make zero low-level calls.

**Actual:** Dereferences `node->size` before any null check and terminates with signal 11 (`status=139`).

**Severity:** High

**Regression test:** `pbt-native/inode_truncate_baseline_pbt_test.c`

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-build --target inode_truncate_baseline_pbt_test -j$(nproc)
/tmp/specfs-pbt-build/inode_truncate_baseline_pbt_test
```

## Root Cause

`inode_truncate` compares `size` with `node->size` and later assigns `node->size = size` without first checking whether `node` is `NULL`.
