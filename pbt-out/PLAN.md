# PBT Campaign: free_readret baseline

## Scan findings
- **Spec:** `eval/loc/spec/util/free_readret.spec:17` documents the valid input preconditions and post-condition: free `p->buf` and then `p` itself.
- **Test layout:** theft-based C property tests live in `pbt-native/*_pbt_test.c`; targets are registered in `pbt-native/CMakeLists.txt` with `add_executable`, `target_include_directories`, `target_link_libraries(... theft)`, and `add_test`.
- **Candidate modules:** `eval/extent/baseline/util.c::free_readret` — Algebraic invariant over allocator/deallocation events.
- **Skipped modules:** (none; user requested one function)

## Module: free_readret_baseline
- [x] Scan: identify targets
- [ ] Plan: formalize properties
- [ ] Test: write and run
- [ ] Review: triage results
