| Function | Source file | Test file | Test target | Notes |
|---|---|---|---|---|
| `hash_func` | `eval/delay_alloc/optimization/hashing.c` | `pbt-native/hash_func_pbt_test.c` | `hash_func_pbt_test` | PASS: 4 properties (output range [0,511], determinism, no-mutation, empty-string-is-0). Uses theft (C PBT framework). |
| `inode_write` | `eval/extent/optimization/inode_management.c` | `pbt-native/inode_write_pbt_test.c` | `inode_write_pbt_test` | 3 PASS, 1 FAIL: unsigned underflow in `MAX_FILE_SIZE - offset` allows writes past file size limit; `max_write < 0` guard is dead code (unsigned). See `pbt-out/bug_reports/inode_write_unsigned_underflow.md`. |
