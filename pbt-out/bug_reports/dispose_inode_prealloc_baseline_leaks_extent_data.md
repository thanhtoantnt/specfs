# dispose_inode pre_alloc baseline payload leak

## Summary

`dispose_inode` in `eval/pre_alloc/baseline/util.c` frees each `Extent` list node for non-directory inodes, but it does not free the `Extent::data` payload owned by each node.

## Property

Formal: `∀ inode. inode.mode != DIR_MODE ∧ inode.extents = [e1..en] ⇒ dispose_inode(inode)` frees the inode, destroys its mutex, frees every extent node exactly once, and frees every `ei.data` exactly once.

Oracle: Algebraic destructor ownership/lifecycle invariant.

## Evidence

The theft property target `dispose_inode_prealloc_baseline_pbt_test` wraps the real `dispose_inode` implementation and tracks allocator events. Directory and empty non-directory destructor properties pass, while extent-backed non-directory inputs fail because payload free counts remain zero.

Failing command:

```sh
cmake --build /tmp/specfs-pbt-prealloc-build --target dispose_inode_prealloc_baseline_pbt_test -j2 && /tmp/specfs-pbt-prealloc-build/dispose_inode_prealloc_baseline_pbt_test
```

Representative counterexample:

```text
== FAIL 'extent_inode_releases_nodes_and_payloads': pass 0, fail 44, skip 4, dup 2
Argument 0:
{extent_count=8, mode_selector=1, payload_sizes=[129,428,1,4096,126,430,338,246]}
dispose_inode extent cleanup mismatch: extents=8 mutex=1 inode=1 first_data_free=0 unknown=0
```

## Root Cause

The extent cleanup loop calls `free(current)` but never calls `free(current->data)` before releasing the node.

## Suggested Fix

Free each extent payload before freeing the extent node:

```c
free(current->data);
free(current);
```
