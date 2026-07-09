# Bug: dispose_inode leaks sparse inline-data pages

**Law:** For every non-directory inode, `dispose_inode(inode)` must free every non-NULL page pointer owned by `inode->file->index`, free the index table, destroy the mutex, and free the inode exactly once.

**Impact:** Sparse inline-data file tables can retain allocated data pages after inode disposal. This leaks file data memory whenever allocated pages are not stored in a prefix ending at the first NULL slot.

**Function:** `dispose_inode` in `eval/inline_data/baseline/util.c`

**Detected by:** Algebraic invariant - destructor ownership/lifecycle

**Minimal input:** A non-directory inode with `file->index[0] == NULL` and one owned page at a later slot, for example `page_count=1, first_sparse_slot=1`.

**Expected:** The destructor scans the whole `INDEXTB_NUM` table and frees every non-NULL `index[i]` exactly once before freeing `inode->file`.

**Actual:** The destructor stops at the first NULL slot:

```c
walk = inum->file->index[i];
if(walk==NULL)
    break;
```

For sparse tables, no later page pointer is freed. The theft property reports `first_page_free=0` while mutex, file table, and inode frees are observed.

**Severity:** medium

**Regression test:** `pbt-native/dispose_inode_inline_data_baseline_pbt_test.c`

**Failing command:**

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-dispose-inline-data-baseline -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-dispose-inline-data-baseline --target dispose_inode_inline_data_baseline_pbt_test -j2
ctest --test-dir /tmp/specfs-pbt-dispose-inline-data-baseline -R dispose_inode_inline_data_baseline_pbt_test --output-on-failure
```

Representative counterexample:

```text
== FAIL 'sparse_pages_are_released': pass 0, fail 131, skip 19, dup 0
Argument 0:
{page_count=2, mode_selector=1, first_sparse_slot=8, sparse_stride=5}
dispose_inode sparse page cleanup mismatch: pages=2 first_slot=8 mutex=1 file=1 inode=1 first_page_free=0 unknown=0
```

**Suggested fix:** Replace the early-terminating loop with a full table scan:

```c
for (i = 0; i < INDEXTB_NUM; i++) {
    walk = inum->file->index[i];
    if (walk != NULL) {
        free(walk);
    }
}
```
