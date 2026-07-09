# Bug Report: pre_alloc inode_write accepts out-of-range offsets

## Target
- `eval/pre_alloc/optimization/inode_management.c`
- Function: `inode_write`

## Finding
`inode_write` computes `unsigned max_write = MAX_FILE_SIZE - offset` before validating that `offset <= MAX_FILE_SIZE`. For `offset > MAX_FILE_SIZE`, unsigned subtraction wraps to a large positive value, and the later check `if (max_write < 0)` is ineffective because `max_write` is unsigned.

As a result, out-of-range writes can:
- return a non-zero written length,
- call `file_write` with an invalid offset,
- update `node->size` beyond `MAX_FILE_SIZE` or to a wrapped high value.

## Reproducer
The property test `pbt-native/inode_write_prealloc_pbt_test.c` generates offsets around `MAX_FILE_SIZE` and `UINT32_MAX`.

Command:

```sh
cmake -S pbt-native -B pbt-native/build && \
cmake --build pbt-native/build --target inode_write_prealloc_pbt_test && \
ctest --test-dir pbt-native/build -R inode_write_prealloc_pbt_test --output-on-failure
```

Example counterexample:

```text
out_of_range_noop failed: actual=445 calls=1 size=33555104 initial=11695568
Argument 0:
{initial_size=11695568, offset=33554659, len=445}
```

`MAX_FILE_SIZE` is `33554432`, so `offset=33554659` is invalid but still writes.

## Expected Contract
For `offset >= MAX_FILE_SIZE`, `inode_write` should return `0`, not call `file_write`, and leave `node->size` unchanged. For valid offsets, it should clamp the effective length to `MAX_FILE_SIZE - offset` and never increase `node->size` beyond `MAX_FILE_SIZE`.

## Suggested Fix
Validate the offset before subtracting:

```c
if (offset >= MAX_FILE_SIZE) {
    return 0;
}
unsigned max_write = MAX_FILE_SIZE - offset;
```
