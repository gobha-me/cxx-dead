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
were false positives because the project call graph reached `make_unique` while the actual
constructor call lived in a system template body outside the project graph. Version 0.9.0 resolves
the named regressions by treating top-level prvalue `unique_ptr<T>`/`shared_ptr<T>` factory results
as conservative construction and destruction evidence. Exact standard factories are recognized;
other owning-pointer factories retain the element lifetime with an explicit diagnostic.

### Callable objects and callbacks

Several lambda `operator()` definitions remain unreachable because the current graph does not connect
callable objects passed through standard containers/algorithms to their invocation sites. Their line
mapping can also be zero. These findings must not receive high-confidence policy treatment.

### Presentation ambiguity

Overloads such as the one-argument and two-argument `Terminal::select_driver` are distinguished by
mangled key, but terminal/JSON symbol labels omit the signature. One overload may therefore be live
while an identically displayed overload is reported. Version 0.4.0 addresses this presentation gap
by adding complete signatures and precise spelling/expansion source extents to reports; callable
reachability remains a separate modeling limitation.

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

1. Model callable escape/invocation.
2. Rerun TermForge after callable modeling and review the remaining callable
   cases.
3. Separate graph scope from report scope so a library can participate in reachability while only an
   executable directory is reported.
4. Add conservative library rooting and rerun TermForge to isolate genuinely unreachable internal
   implementation.
5. Replace DOM AST parsing with streaming LibTooling/clangd fact extraction before analyzing the
   larger neighboring repositories.

## v0.5.0 frontend comparison

The LibTooling prototype was compared with AST JSON on TermForge revision `6ef2825` using the same
48-command database, `termforge` declaration filter, and `termforge::forge_top::run_cli` root.

| Frontend | Wall time | Peak RSS | AST bytes | Fact bytes | Defined / graph symbols | Edges | Findings |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| AST JSON | 67.6 s | 274.1 MiB | 528,097,142 | 4,954,584 | 1,645 / 1,701 | 3,321 | 595 |
| LibTooling | 51.8 s | 275.1 MiB | 0 | 4,990,166 | 1,646 / 1,706 | 3,173 | 607 |

The direct frontend removed the 528 MB intermediate stream and improved wall time by about 23%; the
filtered workload's peak RSS was effectively unchanged. LibTooling's finding set contains all 595
AST JSON candidates plus 12 additional functions, primarily template-adjacent helpers and lambdas.
Golden and scope fixtures remain exact parity gates, but neither TermForge set is deletion evidence
and the larger LibTooling count is not treated as a precision claim.

## v0.9.0 construction regression rerun

The bounded AST JSON frontend reran the same 48-command `forge-top` configuration on TermForge
revision `82b1466` with Clang 20.1.8. The run completed in 71.5 seconds at 279,496 KiB peak RSS,
producing 530,628,637 AST bytes, 7,677,911 fact bytes, 1,698 defined symbols, 1,758 graph symbols,
3,278 edges, and 648 advisory findings. The count is not directly comparable with the v0.5.0 trial
because TermForge changed substantially between revisions.

Neither `FakeReader` nor `ProcReader` constructors remained in the findings. Four project-specific
owning-pointer helpers (`make_driver`, `make_fake_reader`, `make_proc_reader`, and
`select_driver_for`) emitted conservative diagnostics instead of high-confidence constructor
findings. The run remained complete under a 60-second per-TU timeout, 600-second index timeout, and
512 MiB per-TU AST-output limit.

## v0.10.0 callable regression rerun

The callable-aware AST JSON frontend reran the same 48-command `forge-top` configuration on
TermForge revision `82b1466` with Clang 20.1.8. The bounded run completed in 69.1 seconds at
279,072 KiB peak RSS, producing 530,628,637 AST bytes, 7,703,237 fact bytes, 1,698 defined symbols,
1,758 graph symbols, 3,281 edges, and 647 advisory findings.

Of 141 unreachable `operator()` findings, 139 now retain typed callable-object escape evidence and
are classified `dynamically_referenced` rather than high-confidence dead. The two remaining
`likely_dead` call operators are `KittyDriver::ResidentPlacementKeyHash` and
`forge_top::ProcessIdentityHash`; they remain review candidates rather than deletion evidence.
Neither `FakeReader` nor `ProcReader` construction regressed. The run used the same 60-second per-TU,
600-second index, and 512 MiB per-TU AST-output bounds as the v0.9.0 rerun.
