# calculate crashes when realloc fails

## Target
- File: `eval/extent/optimization/path_handling.c`
- File: `eval/extent/baseline/path_handling.c`
- File: `eval/rbtree/optimization/path_handling.c`
- File: `eval/delay_alloc/optimization/path_handling.c`
- File: `eval/inline_data/optimization/path_handling.c`
- Function: `calculate`

## Finding
`calculate` assigns `realloc` directly to `result` and immediately writes through the returned pointer without checking for `NULL`:

- In the loop, a failed growth allocation makes `result == NULL`, then `result[len] = malloc_string(*src)` dereferences `NULL`.
- After the loop, a failed final allocation makes `result == NULL`, then `result[len] = NULL` dereferences `NULL`.

This violates the memory-safety requirement from the file prompt to include proper memory allocation handling.

## Property failure
The property `realloc_failure_does_not_crash` forces the first `realloc` inside `calculate` to fail. Theft immediately finds crashing counterexamples, including:

```text
src=["aaab"], dst=[]
```

For this case, the common prefix is empty, so the final `realloc(result, sizeof(char*) * (len + 1))` fails and `result[0] = NULL` segfaults.

The rbtree variant reproduced the same failure in `calculate_rbtree_pbt_test` with examples including:

```text
src=["d"], dst=[]
```

The delay_alloc variant reproduced the same failure in `calculate_delay_alloc_pbt_test` with examples including:

```text
src=[], dst=["a"]
```

The inline_data variant reproduced the same failure in `calculate_inline_data_pbt_test` with examples including:

```text
src=["aba"], dst=["aba", "", "abdcbd", "bb"]
```

The extent baseline variant reproduced the same failure in `calculate_baseline_pbt_test` with examples including:

```text
src=[], dst=[]
```

## Reproduction
```sh
cmake -S pbt-native -B pbt-native/build
cmake --build pbt-native/build --target calculate_pbt_test
ctest --test-dir pbt-native/build -R '^calculate_pbt_test$' --output-on-failure

cmake --build pbt-native/build --target calculate_baseline_pbt_test
ctest --test-dir pbt-native/build -R '^calculate_baseline_pbt_test$' --output-on-failure

cmake --build pbt-native/build --target calculate_rbtree_pbt_test
ctest --test-dir pbt-native/build -R '^calculate_rbtree_pbt_test$' --output-on-failure

cmake --build pbt-native/build --target calculate_delay_alloc_pbt_test
ctest --test-dir pbt-native/build -R '^calculate_delay_alloc_pbt_test$' --output-on-failure

cmake --build pbt-native/build --target calculate_inline_data_pbt_test
ctest --test-dir pbt-native/build -R '^calculate_inline_data_pbt_test$' --output-on-failure
```

Observed result: the first four functional properties pass, and `realloc_failure_does_not_crash` fails.

## Suggested fix
Store `realloc` in a temporary pointer, check for `NULL`, free any previously allocated result strings on failure, and return `NULL` or another documented error indicator without dereferencing the failed allocation result.
