# inode_truncate skips allocate+clear when growing (spec Case 2 violation)

## Status
Confirmed — failing theft property `grow_clears_new_region` with shrunk witnesses
against the REAL compiled `eval/extent/optimization/inode_management.c::inode_truncate`.

## Property
`inode_truncate_pbt_test.grow_clears_new_region` (spec-contract oracle).

The Coq spec's `inode_truncate.spec` Case 2 states:
> If `size` >= current size, allocate additional space in the file and clear the
> newly allocated space.

The growth path must call `clear_file` (and `file_allocate`) to zero the new region
`[old_size, new_size)`. The optimization only sets `node->size = size` without
calling either function.

## Shrunk Witnesses
```
{old_size=239586, new_size=458729}  → clear_file called 0 times (expect 1)
{old_size=152608, new_size=345055}  → clear_file called 0 times (expect 1)
{old_size=691687, new_size=744244}  → clear_file called 0 times (expect 1)
```

## Root Cause
`eval/extent/optimization/inode_management.c:195`:

```c
void inode_truncate(struct inode* node, unsigned size) {
    if (node == NULL) return;
    unsigned old_size = node->size;
    if (size >= old_size) {
        node->size = size;   // ← BUG: only updates size, no allocate + clear
        return;
    }
    clear_file(node, size, old_size - size);
    node->size = size;
}
```

The `size >= old_size` branch only sets `node->size` and returns. The spec requires
`file_allocate(node->file, old_size, size - old_size)` followed by
`clear_file(node, old_size, size - old_size)`. Neither is called.

The baseline correctly does:
```c
if (size > node->size) {
    file_allocate(node->file, node->size, size - node->size);
    clear_file(node, node->size, size - node->size);
}
```

## Effect
- Growing a file via `inode_truncate` updates the size field but does NOT allocate
  or zero the new pages. A subsequent read of the grown region hits unallocated
  pages, returning garbage data or crashing (NULL deref) depending on the
  lowlevel_file implementation's NULL-page handling.
- Callers that rely on the spec contract (grow → zeroed region) get uninitialized
  data silently.

## Severity
**High** — data loss / NULL deref on file growth. The spec explicitly requires
allocation + clearing; the implementation skips both. Triggered by any truncate
call with `size > node->size`.

## Fix
Add `file_allocate` + `clear_file` to the growth branch:

```c
if (size >= old_size) {
    if (size > old_size) {
        file_allocate(node->file, old_size, size - old_size);
        clear_file(node, old_size, size - old_size);
    }
    node->size = size;
    return;
}
```

## Reproduction
```bash
cd ~/evaluation/specfs
cmake -B pbt-native/build -S pbt-native -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build pbt-native/build -j$(nproc)
(cd pbt-native/build && ctest -R inode_truncate_pbt_test --output-on-failure)
# grow_clears_new_region fails: 0 clear_file calls, expect 1
```
