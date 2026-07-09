# inode_write offset bounds bug

Target: `eval/rbtree/optimization/inode_management.c`, function `inode_write`.

## Finding

`inode_write` does not reject offsets beyond `MAX_FILE_SIZE`. It computes:

```c
unsigned max_write = MAX_FILE_SIZE - offset;
if (max_write < 0) {
    return 0;
}
```

Because `max_write` is unsigned, `max_write < 0` is always false. When `offset > MAX_FILE_SIZE`, the subtraction underflows and can produce a large positive remaining length, causing `inode_write` to call `file_write` and return a nonzero write length for an out-of-range offset.

## Property failure

Command:

```sh
cmake --build pbt-native/build --target inode_write_pbt_test && ./pbt-native/build/inode_write_pbt_test
```

Failing property: `return_is_clamped`.

Example counterexample:

```text
initial_size=29556125, offset=4294967243, len=380
actual return=380, expected return=0
```

The test expects out-of-range offsets (`offset >= MAX_FILE_SIZE`) to be a no-op returning `0`, matching the prompt requirement to include bounds checking for `offset <= MAX_FILE_SIZE` and preventing unsigned wraparound into low-level writes.

## Suggested fix

Check `offset >= MAX_FILE_SIZE` before subtracting:

```c
if (offset >= MAX_FILE_SIZE) {
    return 0;
}
unsigned max_write = MAX_FILE_SIZE - offset;
```
