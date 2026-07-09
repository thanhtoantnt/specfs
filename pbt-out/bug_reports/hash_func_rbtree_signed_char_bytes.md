# hash_func treats high-bit filename bytes as signed chars

## Bugs Found

`eval/rbtree/optimization/hashing.c::hash_func` adds `*name` directly to the hash accumulator. On this platform `char` is signed, so bytes >= 0x80 are interpreted as negative values before the final `& 0x1ff` mask. This violates the byte-oriented multiplicative hash contract from the file's embedded spec (`nat_of_ascii c`) and makes hashes platform-dependent across signed-char vs unsigned-char C implementations.

## Failing Property

- **Test:** `hash_func_rbtree_pbt_test`
- **Property:** `matches_unsigned_byte_recurrence`
- **Formal:** For all valid NUL-terminated byte strings `s` with no interior NUL, `hash_func(s) == foldl((h, b) -> (h * 131 + b) mod 512, 0, s)` where each byte `b` is interpreted as unsigned in `[0, 255]`.
- **Oracle:** Reference recurrence over `unsigned char` bytes.
- **Counterexample:** `len=3 bytes="j\x14\xfd"` produced `expected 243, got 499` during the theft run.

## Reproduction

```sh
cmake -S pbt-native -B /tmp/pbt-native-specfs-hash-rbtree -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/pbt-native-specfs-hash-rbtree --target hash_func_rbtree_pbt_test -j2
/tmp/pbt-native-specfs-hash-rbtree/hash_func_rbtree_pbt_test
```

## Root Cause

At `eval/rbtree/optimization/hashing.c:57`, `hash = hash * 131 + *name;` uses the promoted value of `char`. When `char` is signed, a byte such as `0xfd` contributes `-3` rather than `253`, shifting the final 9-bit hash by 256 from the unsigned-byte specification.

## Suggested Fix

Cast each input byte before adding it:

```c
hash = hash * 131 + (unsigned char)*name;
```
