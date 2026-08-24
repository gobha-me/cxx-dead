# Implementation plan

The roadmap is organized around risk retirement rather than feature count. Each milestone has an explicit contract and acceptance gate.

## Milestone 0 — corpus and measurement

Status: the initial golden corpus, run-state contract, and reviewed result ledger were established
for v0.1.1. The corpus now covers 39 definitions, including overload signatures, macro
spelling/expansion extents, and a line-resolved lambda. Additional fixtures and fully enumerated
real-project reviews remain ongoing as new frontend behavior is added.

Goal: make precision measurable before expanding language coverage.

Deliverables:

- 20–50 small golden fixtures covering direct calls, overloads, namespaces, internal linkage, multiple translation units, constructors/destructors, virtual calls, callbacks, templates, static initialization, macros, and generated paths;
- 3–5 real application repositories with manually reviewed findings;
- a result ledger recording true positives, false positives, known false negatives, indexing time, peak memory, AST bytes, symbol count, and edge count;
- definitions for `complete`, `incomplete`, and `unsupported` analysis runs.

Gate: every high-confidence fixture finding has an explanation and no known live symbol is classified high-confidence dead.

## Milestone 1 — application reachability prototype

Status: implemented in this repository at experimental quality.

Contract:

- one compilation database is treated as one executable configuration;
- every listed translation unit is assumed linked;
- `main`, global-initializer calls, and configured symbols are roots;
- direct calls, construction/destruction, and conservative known override edges propagate reachability;
- address-taking records escape evidence but does not itself imply a call;
- only project-owned function definitions are reported;
- any indexing failure fails the run;
- human and schema-versioned JSON output are available.

Acceptance checks:

- cross-TU live functions are retained;
- an unreachable cycle is reported as one SCC;
- an abandoned type's member findings are aggregated;
- an address-escaped callback is not classified as definitely dead;
- a reachable virtual base call retains a known override;
- internal-linkage candidates receive stronger evidence;
- tests and a manual fixture CLI run pass under Clang.

Remaining hardening tasks:

1. Add full implicit-call fixture coverage, including member/base initialization and cleanup paths.
2. Limit global-initializer roots to project-owned, linked definitions.
3. Extend the structured evidence model with provider-composed dynamic edges and suppressions.
4. Add weak-component/type/directory aggregation above SCCs.
5. Replace provisional confidence numbers with configurable evidence policy.
6. Add resource limits, cancellation, and structured incomplete-run diagnostics.
7. Test command normalization with launchers, response files, modules, PCH, and compiler-specific flags.

## Milestone 2 — scalable Clang frontend

Goal: make application analysis viable on real repositories.

Deliverables:

- LibTooling or clangd-index fact extraction without materializing complete JSON ASTs;
- Clang USRs plus linkage/configuration-aware stable symbol keys;
- streaming per-TU fact files;
- content and command hashing;
- invalidation through header dependency data;
- bounded parallel indexing and deterministic graph merging;
- timing and memory telemetry.

Gate: analyze a one-million-line application within an agreed CI budget, with incremental runs proportional to affected translation units and no precision regression against the corpus.

## Milestone 3 — target-aware application model

Goal: stop treating the compilation database as a link graph.

Deliverables:

- CMake File API adapter first, followed by explicit manifest fallback;
- executable, object-library, shared-library, and static-archive membership;
- link inclusion semantics and target-scoped roots;
- production/test/tool target policies;
- configuration identity and multi-configuration reporting.

Gate: a mixed repository with two executables and shared/internal libraries reports target-relative and workspace-relative reachability without cross-target false positives.

## Milestone 4 — provider API and dynamic systems

Goal: model project-specific reachability without embedding framework assumptions in the core.

Provider output should be structured facts:

```text
root(symbol, provider, evidence)
edge(from, to, kind, provider, evidence)
escape(symbol, mechanism, provider, evidence)
suppression(scope, reason, provider)
```

Initial providers:

- YAML configuration;
- callback-argument registration patterns;
- export lists and visibility metadata;
- generated-code/reporting policy;
- optional Qt/plugin adapters after corpus evidence justifies them.

Gate: a plugin fixture moves from `possibly_dead` to externally/dynamically reachable with an explanation identifying the responsible provider.

## Milestone 5 — library context

Goal: conservatively retain externally consumable source.

Deliverables:

- shared-library exports, symbol version scripts, visibility attributes, module exports, installed headers, and explicit API roots;
- separate public-API, internal-live, and internal-unreachable classifications;
- header-only template policy;
- consumer-manifest support for static libraries and SDKs.

Gate: public APIs with zero internal callers are retained, while unreachable non-exported implementation islands remain reportable.

## Milestone 6 — differential and CI analysis

Goal: prevent new dead-code debt without requiring historical cleanup.

Prerequisites are stable IDs, stable schema, target/configuration identity, and reproducible baselines.

Deliverables:

- baseline artifact comparison before Git checkout orchestration;
- `new_symbol`, `newly_unreachable`, `removed`, and `became_reachable` distinctions;
- classification/confidence thresholds;
- SARIF output;
- GitHub annotations and agent-oriented compact JSON;
- explicit baseline-incomplete behavior.

Gate: CI reliably fails only on newly introduced policy-matching findings and explains reachability changes.

## Milestone 7 — architectural reporting

Goal: elevate symbol facts into maintainable subsystem insights.

Deliverables:

- unreachable condensation DAG;
- weak-component grouping with type/file/directory ownership hints;
- estimated non-overlapping source ranges;
- reachability-path and missing-incoming-edge explanations;
- optional coverage/profile evidence that never overrides static safety by itself.

Gate: reviewed reports identify abandoned components with materially less noise than flat symbol lists.

## Suggested issue order

For the next implementation cycle:

1. Add project-owned global initialization modeling.
2. Add unreachable weak-component aggregation.
3. Prototype a LibTooling fact collector and benchmark it against AST JSON.
4. Define stable graph artifact and JSON schemas.
5. Add CMake File API target ingestion.

Do not start library inference or baseline CI policy until target identity and index completeness are reliable.
