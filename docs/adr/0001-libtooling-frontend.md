# ADR 0001: Choose LibTooling for scalable fact extraction

Status: accepted on 2026-08-26 for the v0.5.0 experimental frontend.

## Context

The AST JSON prototype materializes complete Clang dump documents. Null Vector emitted 5.03 GB of
AST JSON for nine translation units and peaked at 3.52 GiB. The frontend boundary must preserve the
existing graph facts and fail-closed contract without making Clang development packages mandatory
for every build.

The evaluated directions were direct LibTooling AST traversal and reuse of clangd indexing. The
clangd index is optimized for symbol/reference lookup and would still require an AST pass for exact
call, construction/destruction, escape, initializer-root, and source-extent facts. It also adds a
larger internal dependency surface. LibTooling exposes those semantics in one `FrontendAction` and
was available across Clang 18 and 20.

## Decision

Use LibTooling as the scalable frontend direction. Keep it opt-in behind
`CXX_DEAD_ENABLE_LIBTOOLING`, expose it as `--frontend libtooling`, and retain AST JSON as the
dependency-light default while the direct collector is experimental.

Both frontends emit a per-translation-unit neutral `Graph` fact batch and use the same merger,
manual-root processing, traversal, classification, and reporting. The measurement-only fact byte
count is not itself a durable artifact schema. Version 0.6.0 adds the stable identity and graph
artifact contract; per-TU persistence and invalidation remain follow-up work.

## Evidence and consequences

- Golden and scope fixtures produce identical reports and typed edges across both frontends.
- On TermForge's 48-TU filtered trial, LibTooling completed in 51.8 s versus 67.6 s for AST JSON;
  both peaked near 275 MiB. The direct collector produced 4.99 MB of neutral fact payload without
  writing the 528 MB AST stream.
- On Null Vector's unfiltered nine-TU trial, LibTooling completed in 8.0 s at 226 MiB versus
  128.1 s at 3.52 GiB and avoided 5.03 GB of AST output.
- LibTooling builds require matching LLVM/Clang headers and libraries and track their C++ API.
- The current visitor records explicit specializations but does not traverse implicit
  specialization bodies; deeper template-mediated call modeling remains follow-up work.
