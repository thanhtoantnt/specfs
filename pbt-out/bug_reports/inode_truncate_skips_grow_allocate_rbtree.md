# inode_truncate growth does not clear newly exposed bytes

Target: `eval/rbtree/optimization/inode_management.c`, function `inode_truncate`.

## Finding

When `inode_truncate(node, size)` grows an inode (`size > node->size`), the implementation only assigns `node->size = size` and returns. It does not call `clear_file` for the newly exposed byte range `[old_size, size)`.

## Why this matters

The prompt contract says `inode_truncate` should update size while simulating block effects via `clear_file`. For truncation growth, bytes newly visible to later reads should be zeroed/cleared; otherwise stale or uninitialized backing data can become observable.

## Property that fails

`pbt-native/inode_truncate_pbt_test.c` includes `grow_clears_new_region`, which expects exactly one `clear_file(node, old_size, size - old_size)` call for `size > old_size`.

Example failing case from the PBT run:

```text
old_size=409851, new_size=523310
expected clear_file start=409851 len=113459
actual clear_file calls=0
```

## Reproduction

```sh
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target inode_truncate_pbt_test -j2
./pbt-native/build/inode_truncate_pbt_test
```

Observed result: `size_always_updates`, `shrink_clears_tail`, and `null_is_safe` pass; `grow_clears_new_region` fails.
