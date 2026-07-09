# hash_func pre_alloc treats high-bit filename bytes as signed chars

## Bugs Found

`eval/pre_alloc/optimization/hashing.c::hash_func` adds `*name` directly to the hash accumulator. On this platform `char` is signed, so bytes >= `0x80` are interpreted as negative values before the final `& 0x1ff` mask. That makes the hash depend on the host compiler's `char` signedness and violates the byte-oriented multiplicative hash contract from the embedded spec (`nat_of_ascii c`).

## Failing Property

- **Test:** `hash_func_prealloc_pbt_test`
- **Property:** `last_byte_extends_prefix`
- **Formal:** For every NUL-terminated byte string `s`, if `s = p ++ [b]` then `hash_func(s) == ((hash_func(p) * 131) + b) mod 512`, with `b` interpreted as an unsigned byte in `[0, 255]`.
- **Oracle:** Reference recurrence over `unsigned char` bytes.
- **Counterexample:** `len=1 bytes="\xfb"` produced `expected 251, got 507`.

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-hash-prealloc-pbt -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-hash-prealloc-pbt --target hash_func_prealloc_pbt_test -j$(nproc)
/tmp/specfs-hash-prealloc-pbt/hash_func_prealloc_pbt_test
```

## Root Cause

At `eval/pre_alloc/optimization/hashing.c:57`, `hash = hash * 131 + *name;` uses the promoted value of `char`. When `char` is signed, a byte such as `0xfb` contributes `-5` rather than `251`, shifting the final 9-bit hash by 256 from the unsigned-byte specification.

## Suggested Fix

Cast each input byte before adding it:

```c
hash = hash * 131 + (unsigned char)*name;
```
