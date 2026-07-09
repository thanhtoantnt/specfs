| Function | Source file | Test file | Test target | Notes |
|---|---|---|---|---|
| `hash_func` | `eval/delay_alloc/optimization/hashing.c` | `pbt-native/hash_func_pbt_test.c` | `hash_func_pbt_test` | PASS: 4 properties (output range [0,511], determinism, no-mutation, empty-string-is-0). Uses theft (C PBT framework). |
