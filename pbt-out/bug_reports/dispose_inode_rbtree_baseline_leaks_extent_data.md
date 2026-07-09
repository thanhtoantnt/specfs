# dispose_inode rbtree baseline extent payload leak

## Summary

`dispose_inode` in `eval/rbtree/baseline/util.c` frees each `Extent` list node for non-directory inodes, but it does not free the `Extent::data` payload owned by each node.

## Property

Formal: `forall inode. inode.mode != DIR_MODE && inode.extents = [e1..en] => dispose_inode(inode)` frees the inode, destroys its mutex, frees every extent node exactly once, and frees every `ei.data` exactly once.

Oracle: Algebraic destructor ownership/lifecycle invariant.

## Evidence

The theft property target `dispose_inode_rbtree_baseline_pbt_test` wraps the real `eval/rbtree/baseline/util.c` implementation and tracks allocator events. Directory, empty non-directory, and prealloc cleanup properties pass. Extent-backed non-directory inputs fail because payload free counts remain zero.

Failing command:

```sh
cmake -S pbt-native -B /tmp/pbt-native-rbtree-baseline -G 'Unix Makefiles' && cmake --build /tmp/pbt-native-rbtree-baseline --target dispose_inode_rbtree_baseline_pbt_test -j2 && ctest --test-dir /tmp/pbt-native-rbtree-baseline --output-on-failure -R '^dispose_inode_rbtree_baseline_pbt_test$'
```

Representative counterexample:

```text
== FAIL 'extent_inode_releases_nodes_and_payloads': pass 0, fail 106, skip 13, dup 1
Argument 0:
{extent_count=2, prealloc_count=2, mode_selector=4}
dispose_inode extent cleanup mismatch: extents=2 mutex=1 inode=1 first_data_free=0 unknown=0
```

## Root Cause

The extent cleanup loop calls `free(current)` but never calls `free(current->data)` before releasing the node.

## Suggested Fix

Free each extent payload before freeing the extent node:

```c
free(current->data);
free(current);
```
