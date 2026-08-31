# Roadmap

This roadmap is ordered by correctness and architectural risk. Dates are intentionally omitted
until the prototype has stable performance and precision measurements.

## M1 — Application-mode hardening

Status: v0.16.0 limits automatic global-initializer roots to project-owned code in the selected
link model, above the v0.15.0 topology and v0.10.0 callable foundations. Macro and broader
language-modeling work remain open.

- Separate symbols needed for graph traversal from project-owned symbols eligible for reporting.
- Represent roots, escapes, and classifications as structured evidence.
- Improve implicit construction and destruction coverage. (initial factory/lifetime coverage
  completed in v0.9.0)
- Improve lambda, callable, macro, and source-range coverage. (initial callable and registration
  coverage completed in v0.10.0)
- Limit global-initializer roots to project-owned linked definitions. (completed in v0.16.0)
- Aggregate unreachable SCCs with type/file/directory ownership hints. (completed in v0.15.0)
- Add resource limits and explicit incomplete-run diagnostics. (completed in v0.8.0)
- Expand the golden fixture corpus and reviewed real-project result ledger.

Exit criterion: no known live symbol in the application corpus is classified as high-confidence
dead, and every high-confidence finding has a useful explanation.

## M2 — Scalable indexing

Status: v0.13.0 adds dependency-validated deterministic per-TU fact caching for both frontends and
separates cache validation, indexing, merging, traversal, SCC, and reporting telemetry. Bounded
parallelism and large-application budget validation remain open.

- Promote the measured LibTooling prototype from its optional frontend into the scalable indexing
  path without removing the dependency-light AST JSON fallback.
- Maintain the v0.6.0 configuration-aware cross-translation-unit identities and graph artifact
  schema as compatibility contracts.
- Store deterministic per-TU facts with command/content hashing. (completed in v0.13.0)
- Add bounded parallelism, incremental invalidation, and resource telemetry. (incremental
  invalidation and telemetry completed in v0.13.0; parallelism remains open)

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

Status: v0.12.0 adds conservative source-level shared/static library API rooting, public-header and
visibility evidence, and explicit public API roots on top of the v0.11.0 provider foundation.

- Expand the typed provider foundation from callback-registration edges to general roots, dynamic
  edges, escapes, and suppressions. (completed in v0.11.0)
- Add YAML configuration on top of the v0.10.0 repeatable CLI registration rule. (completed in
  v0.11.0)
- Conservatively infer shared-library exports, public headers, visibility, and explicit APIs.
  (completed in v0.12.0 for source metadata)
- Define policies for templates and header-only libraries. (initial observed-template policy
  completed in v0.12.0; consumer-context analysis remains open)

Exit criterion: public API with no internal consumers is retained while unreachable private
implementation islands remain reportable.

## M5 — Differential and CI analysis

Status: v0.15.2 documents and validates the routine investigative coding-agent workflow after the
v0.15.1 third-application advisory rollout. GitHub workflow packaging remains open.

- Compare stable graph artifacts across revisions. (completed in v0.14.0 for one explicit baseline)
- Report newly unreachable and newly reachable symbols. (completed in v0.14.0)
- Add policy thresholds, SARIF, and code-review annotations. (policy and SARIF completed in v0.14.0)
- Aggregate symbols into explainable types, files, and subsystems. (completed in v0.15.0)

Exit criterion: CI can fail specifically on newly introduced, policy-matching findings without
depending on historical cleanup.

The detailed engineering plan and acceptance gates live in
[`docs/implementation-plan.md`](docs/implementation-plan.md).
