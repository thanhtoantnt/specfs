# Bug Report: file_write crashes on NULL inode

Target: `eval/extent/optimization/lowlevel_file.c::file_write`

## Summary

`file_write(NULL, offset, len, data)` dereferences the null `node` pointer through `file_allocate(node, offset, len)`, causing a segmentation fault instead of safely returning.

## Why this violates the contract

The generated file's guarantee says memory-safety checks should verify whether a pointer is null before use. `file_write` is part of the low-level file I/O boundary and should therefore handle a null inode as a no-op/error return path rather than crashing.

## Reproducer

The property `null_inode_is_safe` in `pbt-native/lowlevel_file_pbt_test.c` forks a child and calls:

```c
file_write(NULL, 0U, 1U, data);
```

Observed result from `ctest --test-dir pbt-native/build -R lowlevel_file_pbt_test --output-on-failure`:

```text
null inode crashed or returned non-zero: status=139
== FAIL 'null_inode_is_safe'
```

## Root cause

`file_write` immediately calls `file_allocate(node, offset, len)` without checking `node`:

```c
void file_write(struct inode *node, unsigned offset, unsigned len, const char *data) {
    file_allocate(node, offset, len);
```

`file_allocate` then passes `node` to `is_allocated`, which dereferences `node->extents`.

## Suggested fix

Add a null/zero-length guard to `file_write` before calling `file_allocate`; consider adding the same null guard to `file_allocate` and `file_read` for consistency.
