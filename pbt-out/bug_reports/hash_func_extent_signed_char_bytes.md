# hash_func extent treats high-bit filename bytes as signed chars

## Bugs Found

`eval/extent/optimization/hashing.c::hash_func` adds `*name` directly to the hash accumulator. On this platform `char` is signed, so bytes above 127 are promoted as negative values before the final `& 0x1ff` mask. This violates the byte-oriented multiplicative hash contract from the embedded spec (`nat_of_ascii c`) and makes results platform-dependent across signed-char and unsigned-char C implementations.

## Failing Property

- **Test:** `hash_func_extent_highbyte_pbt_test`
- **Property:** `high_bytes_match_unsigned_byte_recurrence`
- **Formal:** For all valid NUL-terminated byte strings `s` with no interior NUL and at least one byte `b > 127`, `hash_func(s) == foldl((h, b) -> (h * 131 + b) mod 512, 0, s)` where every byte is interpreted as unsigned in `[0, 255]`.
- **Oracle:** Reference recurrence over `unsigned char` bytes.
- **Counterexample:** `len=1 high_pos=0 bytes="\xda"` produced `expected 218, got 474` during the theft run.

## Reproduction

```sh
cmake -S pbt-native -B /tmp/specfs-pbt-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/specfs-pbt-build --target hash_func_extent_highbyte_pbt_test -j2
/tmp/specfs-pbt-build/hash_func_extent_highbyte_pbt_test
```

## Root Cause

At `eval/extent/optimization/hashing.c`, `hash = hash * 131 + *name;` uses the promoted value of `char`. When `char` is signed, a byte such as `0xda` contributes `-38` rather than `218`, shifting the final 9-bit hash by 256 from the unsigned-byte specification.

## Suggested Fix

Cast each input byte before adding it:

```c
hash = hash * 131 + (unsigned char)*name;
```
