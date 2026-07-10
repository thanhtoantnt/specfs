## dispose_inode::directory_releases_owned_fields
- Tier: 4
- Rationale: Terminal destructor, so state-machine and differential oracles do not apply. This checks the directory-mode ownership invariant directly from the source behavior in `dispose_inode`: directory inodes own `dir`, and every valid destructor call must destroy the mutex and free the inode exactly once.
- Formal: ∀ inode `i` with `i.mode == DIR_MODE`, `dispose_inode(i)` frees `i.dir` exactly once, destroys `i.impl` exactly once, and frees `i` exactly once.
- Test file: pbt-native/dispose_inode_rbtree_baseline_pbt_test.c
- Status: passing
- Counterexample: (none)
- Bug report: (none)

```property
function: dispose_inode
oracle: algebraic.invariant
predicate:
  quantifier: forall
  vars: [dc]
  domain:
    dc: gen:tuple(int(0..12), int(0..12), int(0..5))
  relation:
    op: holds
    expr: "if node.mode == DIR_MODE then dispose_inode(node) frees node.dir once, destroys mutex once, and frees inode once"
generators:
  dc:
    gen: tuple
    elems:
      - { gen: int, min: 0, max: 12 }
      - { gen: int, min: 0, max: 12 }
      - { gen: int, min: 0, max: 5 }
evidence: eval/rbtree/baseline/util.c:146
```

## dispose_inode::empty_non_dir_releases_inode_and_mutex_only
- Tier: 4
- Rationale: Same ownership invariant, but for a non-directory inode with no extents or prealloc nodes. This rejects the stronger state-machine and differential oracles for the same reasons as above.
- Formal: ∀ inode `i` with `i.mode != DIR_MODE` and `i.extents == NULL` and `i.preallocs == NULL`, `dispose_inode(i)` destroys `i.impl` exactly once, frees `i` exactly once, and does not free a directory table.
- Test file: pbt-native/dispose_inode_rbtree_baseline_pbt_test.c
- Status: passing
- Counterexample: (none)
- Bug report: (none)

```property
function: dispose_inode
oracle: algebraic.invariant
predicate:
  quantifier: forall
  vars: [dc]
  domain:
    dc: gen:tuple(int(0..12), int(0..12), int(0..5))
  relation:
    op: holds
    expr: "if node.mode != DIR_MODE and node.extents == NULL and node.preallocs == NULL then dispose_inode(node) destroys mutex once, frees inode once, and does not free node.dir"
generators:
  dc:
    gen: tuple
    elems:
      - { gen: int, min: 0, max: 12 }
      - { gen: int, min: 0, max: 12 }
      - { gen: int, min: 0, max: 5 }
evidence: eval/rbtree/baseline/util.c:146
```

## dispose_inode::extent_inode_releases_nodes_and_payloads
- Tier: 4
- Rationale: Strongest useful oracle is still an algebraic ownership invariant. The function is terminal, so state-machine and differential oracles are rejected. This property targets the extent list cleanup in the real `dispose_inode` implementation.
- Formal: ∀ inode `i` with `i.mode != DIR_MODE` and `i.extents = [e1..en]`, `dispose_inode(i)` frees every `ei` exactly once, frees every `ei.data` exactly once, destroys `i.impl` exactly once, and frees `i` exactly once.
- Test file: pbt-native/dispose_inode_rbtree_baseline_pbt_test.c
- Status: failing
- Counterexample: `extent_count=2, prealloc_count=2, mode_selector=4`
- Bug report: pbt-out/bug_reports/dispose_inode_rbtree_baseline_leaks_extent_data.md

```property
function: dispose_inode
oracle: algebraic.invariant
predicate:
  quantifier: forall
  vars: [dc]
  domain:
    dc: gen:tuple(int(1..12), int(0..12), int(0..5))
  relation:
    op: holds
    expr: "if node.mode != DIR_MODE and node.extents has at least one element then dispose_inode(node) frees every extent node exactly once and every extent payload exactly once, plus mutex and inode exactly once"
generators:
  dc:
    gen: tuple
    elems:
      - { gen: int, min: 1, max: 12 }
      - { gen: int, min: 0, max: 12 }
      - { gen: int, min: 0, max: 5 }
evidence: eval/rbtree/baseline/util.c:146
```

## dispose_inode::prealloc_inode_releases_nodes_and_payloads
- Tier: 4
- Rationale: Same ownership invariant, now for the prealloc chain and the auxiliary `prealloc_data` array. This is the strongest useful oracle; no independent reference implementation is available.
- Formal: ∀ inode `i` with `i.mode != DIR_MODE` and `i.preallocs = [p1..pm]`, `dispose_inode(i)` frees every `pi` exactly once, frees every `pi.pa_data` exactly once, destroys `i.impl` exactly once, and frees `i` exactly once.
- Test file: pbt-native/dispose_inode_rbtree_baseline_pbt_test.c
- Status: passing
- Counterexample: (none)
- Bug report: (none)

```property
function: dispose_inode
oracle: algebraic.invariant
predicate:
  quantifier: forall
  vars: [dc]
  domain:
    dc: gen:tuple(int(0..12), int(1..12), int(0..5))
  relation:
    op: holds
    expr: "if node.mode != DIR_MODE and node.preallocs has at least one element then dispose_inode(node) frees every prealloc node exactly once and every prealloc payload exactly once, plus mutex and inode exactly once"
generators:
  dc:
    gen: tuple
    elems:
      - { gen: int, min: 0, max: 12 }
      - { gen: int, min: 1, max: 12 }
      - { gen: int, min: 0, max: 5 }
evidence: eval/rbtree/baseline/util.c:146
```
