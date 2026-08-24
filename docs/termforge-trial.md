# TermForge field trial

Date: 2026-08-24

## Scope

The trial analyzed a purpose-built CMake configuration containing the 48 translation units for the
`forge-top` executable and its linked TermForge static library. Tests, examples, benchmarks, and
developer tools were disabled.

Because TermForge is also a public library, the result means "unreachable from forge-top." It does
not mean that an uncalled public library API is dead. The run used Clang's experimental declaration
filter and explicitly modeled the source-level call made by global `main`:

```bash
cxx-dead ../termforge/build-cxx-dead/compile_commands.json \
  --project-root ../termforge \
  --ast-filter termforge \
  --root termforge::forge_top::run_cli \
  --format json
```

## Measurements

The original unfiltered index was stopped after more than eleven minutes. It remained CPU-active,
used roughly 3–3.5 GiB resident memory at its high-water marks, and exposed an algorithmic problem:
system-header definitions were entering the graph and constructor resolution repeatedly scanned that
oversized graph.

After restricting graph declarations and function-body traversal to `--project-root`, a filtered run
completed in 69.96 seconds. Full JSON AST materialization still dominates memory, so this is a useful
prototype mode rather than the intended production indexing architecture.

The final report after identity and template-pattern corrections contained:

```text
defined project symbols     1,618
reachable                   1,033
unreachable candidates        585

dead/internal                  30
likely_dead                   502
possibly_dead                  40
dynamically_referenced         13
```

## What the trial validated

- The target-restricted compilation database indexed without a Clang failure.
- Cross-translation-unit reachability worked for ordinary direct calls.
- `run_cli` provided a usable semantic application root when `main` was excluded by the AST filter.
- Address escapes correctly highlighted signal handlers (`on_winch`, `on_cont`) and SIMD dispatch
  functions rather than calling them definitely dead.
- Overloads had separate mangled identities even though the human-facing symbol spelling was
  ambiguous.
- The JSON report remained small enough to inspect mechanically (about 372 KiB).

## Bugs found in cxx-dead

### Static member identity

The first completed report contained hundreds of duplicate `ImageLayer::above_text`, `below_text`,
and related findings. A `static` class method has external/ODR identity; only a namespace-scope
`static` function has internal linkage. The indexer incorrectly applied the latter rule to both.

This was fixed and covered by a two-translation-unit fixture. The correction reduced the report from
823 to 598 candidates.

### Template patterns

Function-template patterns such as `parse_integer`, `append_le`, and `take_le` appeared dead even
when concrete instantiations were used. The pattern has no mangled identity and should not be treated
as an emitted function definition. The indexer now leaves patterns in the graph but reports only
concrete mangled specializations.

### Template-mediated construction

Constructors invoked inside `std::make_unique`, including TermForge's `FakeReader` and `ProcReader`,
remain false positives. The project call graph reaches `make_unique`, but the actual constructor call
is in a system template body outside the project graph. This needs either explicit standard-library
semantic modeling or instantiation-level call facts from a LibTooling frontend.

### Callable objects and callbacks

Several lambda `operator()` definitions remain unreachable because the current graph does not connect
callable objects passed through standard containers/algorithms to their invocation sites. Their line
mapping can also be zero. These findings must not receive high-confidence policy treatment.

### Presentation ambiguity

Overloads such as the one-argument and two-argument `Terminal::select_driver` are distinguished by
mangled key, but terminal/JSON symbol labels omit the signature. One overload may therefore be live
while an identically displayed overload is reported. Reports should include signatures.

## Interpretation of TermForge findings

Most of the 585 candidates are expected in this context:

- public TermForge APIs not used by the bundled `forge-top` application;
- widgets not used by this particular application;
- explicit test seams such as `run_headless`, `show_first_process_for_test`, and accessors ending in
  `_for_test`;
- test-only helpers such as `ProcessPanel::set_filter`;
- library features such as trace reading, image loading, animation control, and alternative widget
  APIs that remain valid for external consumers.

The trial did not establish a production TermForge function that is safe to remove. That is a useful
precision result: application mode correctly reveals the need for target/library context, while the
remaining extraction gaps prevent treating internal findings as deletion evidence.

## Recommended next work from the trial

1. Model `make_unique`/`make_shared` construction and callable escape/invocation.
2. Add signatures and reliable lambda locations to findings.
3. Separate graph scope from report scope so a library can participate in reachability while only an
   executable directory is reported.
4. Add conservative library rooting and rerun TermForge to isolate genuinely unreachable internal
   implementation.
5. Replace DOM AST parsing with streaming LibTooling/clangd fact extraction before analyzing the
   larger neighboring repositories.
