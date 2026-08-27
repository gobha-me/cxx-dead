# Roadmap

This roadmap is ordered by correctness and architectural risk. Dates are intentionally omitted
until the prototype has stable performance and precision measurements.

## M1 — Application-mode hardening

Status: v0.10.0 adds callable-object escape evidence, direct callable invocation, and configured
callback-registration provenance. Macro and broader language-modeling work remain open.

- Separate symbols needed for graph traversal from project-owned symbols eligible for reporting.
- Represent roots, escapes, and classifications as structured evidence.
- Improve implicit construction and destruction coverage. (initial factory/lifetime coverage
  completed in v0.9.0)
- Improve lambda, callable, macro, and source-range coverage. (initial callable and registration
  coverage completed in v0.10.0)
- Add resource limits and explicit incomplete-run diagnostics. (completed in v0.8.0)
- Expand the golden fixture corpus and reviewed real-project result ledger.

Exit criterion: no known live symbol in the application corpus is classified as high-confidence
dead, and every high-confidence finding has a useful explanation.

## M2 — Scalable indexing

- Promote the measured LibTooling prototype from its optional frontend into the scalable indexing
  path without removing the dependency-light AST JSON fallback.
- Maintain the v0.6.0 configuration-aware cross-translation-unit identities and graph artifact
  schema as compatibility contracts.
- Store deterministic per-TU facts with command/content hashing.
- Add bounded parallelism, incremental invalidation, and resource telemetry.

Exit criterion: a large application can be analyzed within a documented time and memory budget
without precision regressions.

## M3 — Target-aware workspaces

Status: v0.7.0 provides target-scoped CMake File API and explicit-manifest analysis for one selected
configuration and target per run. Workspace aggregation and richer target policy remain open.

- Ingest the CMake File API and provide an explicit target manifest fallback.
- Model executable, object-library, static-library, and shared-library membership.
- Distinguish production, test, and tool reachability policies.

Exit criterion: a mixed workspace reports target-relative and workspace-relative reachability
without treating the compilation database as a single link unit.

## M4 — Providers and library context

- Expand the typed provider foundation from callback-registration edges to general roots, dynamic
  edges, escapes, and suppressions.
- Add YAML configuration on top of the v0.10.0 repeatable CLI registration rule.
- Conservatively infer shared-library exports, public headers, visibility, and explicit APIs.
- Define policies for templates and header-only libraries.

Exit criterion: public API with no internal consumers is retained while unreachable private
implementation islands remain reportable.

## M5 — Differential and CI analysis

- Compare stable graph artifacts across revisions.
- Report newly unreachable and newly reachable symbols.
- Add policy thresholds, SARIF, and code-review annotations.
- Aggregate symbols into explainable types, files, and subsystems.

Exit criterion: CI can fail specifically on newly introduced, policy-matching findings without
depending on historical cleanup.

The detailed engineering plan and acceptance gates live in
[`docs/implementation-plan.md`](docs/implementation-plan.md).
