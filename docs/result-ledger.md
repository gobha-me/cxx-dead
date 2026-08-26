# Reviewed result ledger

This ledger makes correctness and resource observations comparable across revisions. A finding is a
**true positive** when the symbol is genuinely unreachable in the run's stated application context;
it need not be safe to delete from every library or target. A **false positive** is live in that
context but lacks a modeled path. A **known false negative** is deliberately unreachable but absent
from the report. Counts apply only to the reviewed scope shown below; `not captured` preserves an
unknown instead of treating it as zero.

## Run-state contract

| State | Meaning | Report and exit behavior |
| --- | --- | --- |
| `complete` | Every selected translation unit was indexed and an application root exists. | Findings may be emitted; exit 0 for advisory success or 2 under `--fail-on-unreachable`. |
| `incomplete` | A required compile, AST parse, or root-discovery step failed. | The partial graph is discarded, no findings are emitted, and the process exits 1. |
| `unsupported` | The requested analysis context is not implemented. | The request is rejected without a report and exits 1. |

Unsupported language or framework behavior within a complete run is recorded as a limitation; it
does not make a partial graph appear complete. The architecture document is the normative contract.

## Aggregate runs

Measurements are machine- and configuration-specific. Peak RSS is the maximum resident set across
the analyzer and its completed subprocesses. AST bytes are the sum of Clang JSON written to stdout;
fact bytes are an in-memory neutral-fact payload estimate before cross-TU merging.
`Graph symbols` includes declarations as well as reportable definitions.

| Run | Revision / environment | State | Reviewed findings | TP | FP | Known FN | Wall time | Peak RSS | AST / fact bytes | Defined / graph symbols | Edges |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| AST output-limit fixture | v0.8.0 candidate; 128-byte per-TU bound | incomplete | 0 | 0 | 0 | 0 | bounded by fixture | not benchmarked | 128 accepted / 0 committed | 0 committed | 0 committed |
| AST timeout/process-group fixture | v0.8.0 candidate; 75 ms test bound | incomplete | 0 | 0 | 0 | 0 | under 2 s assertion | not benchmarked | 0 accepted / 0 committed | 0 committed | 0 committed |
| Target fixture, production executable | v0.7.0 candidate; CMake File API; Clang 20.1.8 | complete | 4 / 4 | 4 | 0 | 0 | not benchmarked | not benchmarked | not captured | 8 / 8 | 3 |
| Target fixture, test executable | v0.7.0 candidate; CMake File API; Clang 20.1.8 | complete | 4 / 4 | 4 | 0 | 0 | not benchmarked | not benchmarked | not captured | 6 / 6 | 1 |
| Golden corpus, LibTooling | v0.6.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 18 / 18 | 18 | 0 | 0 | 9 ms | 87,796 KiB | 0 / 25,472 | 39 / 40 | 22 |
| Golden corpus, AST JSON | v0.6.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 18 / 18 | 18 | 0 | 0 | 94 ms | 95,484 KiB | 202,187 / 24,193 | 39 / 40 | 22 |
| Golden corpus, LibTooling | v0.5.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 18 / 18 | 18 | 0 | 0 | 8 ms | 125.5 MiB | 0 / 17,575 | 39 / 40 | 22 |
| Golden corpus, AST JSON | v0.5.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 18 / 18 | 18 | 0 | 0 | 71 ms | 125.5 MiB | 202,187 / 17,439 | 39 / 40 | 22 |
| TermForge `forge-top`, LibTooling | v0.5.0 candidate; TermForge `6ef2825`; filtered Clang 20.1.8 | complete | 607 / 607 triaged by category | not individually enumerated | template and callable cases remain | not measured | 51.8 s | 275.1 MiB | 0 / 4,990,166 | 1,646 / 1,706 | 3,173 |
| TermForge `forge-top`, AST JSON | v0.5.0 candidate; TermForge `6ef2825`; filtered Clang 20.1.8 | complete | 595 / 595 triaged by category | not individually enumerated | callable cases remain | not measured | 67.6 s | 274.1 MiB | 528,097,142 / 4,954,584 | 1,645 / 1,701 | 3,321 |
| Null Vector, LibTooling | v0.5.0 candidate; Null Vector `0786d15`; Clang 20.1.8 | complete | 44 / 44 triaged by category | not individually enumerated | framework callback cascade remains | not measured | 8.0 s | 225.6 MiB | 0 / 1,477,703 | 123 / 1,306 | 882 |
| Null Vector, AST JSON | v0.5.0 candidate; Null Vector `0786d15`; Clang 20.1.8 | complete | 44 / 44 triaged by category | not individually enumerated | framework callback cascade remains | not measured | 128.1 s | 3.52 GiB | 5,025,849,020 / 680,475 | 123 / 642 | 470 |
| Golden application corpus | v0.4.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 18 / 18 | 18 | 0 | 0 | 70 ms | 5,080 KiB | 202,187 / not captured | 39 / 40 | 22 |
| Scope-separation fixture | v0.4.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 1 / 1 | 1 | 0 | 0 | 37 ms | 4,676 KiB | 104,266 / not captured | 4 / 9 | 6 |
| Golden application corpus | v0.3.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 17 / 17 | 17 | 0 | 0 | 69 ms | 4,960 KiB | 171,431 / not captured | 38 / 39 | 22 |
| Scope-separation fixture | v0.3.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 1 / 1 | 1 | 0 | 0 | 52 ms | 4,536 KiB | 104,266 / not captured | 4 / 9 | 6 |
| TermForge `forge-top` | v0.1.0-era corrected prototype; filtered Clang run | complete | 585 / 585 triaged by category | not individually enumerated | at least 2 named factory-construction cases plus callable cases | not measured | 69.96 s | approximately 3–3.5 GiB | not captured | 1,618 / not captured | not captured |
| Null Vector application | v0.1.0-era corrected prototype; 9 selected TUs | complete | 44 / 44 triaged by category | not individually enumerated | at least 5 framework callbacks plus their live cascade | not measured | 145.4 s | approximately 3.6 GiB | not captured | 123 / not captured | not captured |
| Invalid golden translation unit | v0.3.0 candidate; Clang 20.1.8; Linux x86-64 | incomplete | 0 | 0 | 0 | 0 | not benchmarked | not benchmarked | 0 accepted / not captured | 0 committed | 0 committed |

The historical field trials predate AST-byte and edge counters. Their missing measurements remain
explicit rather than reconstructed from a different revision. Future reruns should populate every
column and retain the old row when the frontend or run configuration changes.

The v0.5.0 and v0.6.0 frontend rows are single controlled comparison runs after one warm
development run; ordinary scheduler and filesystem-cache noise applies. The field-trial finding
sets remain advisory:
the equal Null Vector count preserves the known framework callback limitation, while TermForge's
frontend count difference reflects template-body traversal differences and is not evidence that
either frontend's unique classification is safe.

## Golden review coverage

The three-translation-unit corpus contains 39 project definitions and 40 graph symbols. Its 39
table-driven expectations cover both live and unreachable outcomes for:

- direct and cross-translation-unit calls, overloads, namespaces, external and internal linkage;
- constructors, destructors, base/member initialization, virtual dispatch, and unused overrides;
- directly called, address-escaped, and unused callbacks;
- used and unused template specializations;
- global initialization, macro-defined functions, exact spelling/expansion ranges, a line-resolved
  lambda, generated sources, and excluded generated paths;
- merged external-linkage header definitions and fail-closed invalid translation units.

Every unreachable expectation asserts its classification and typed evidence chain. Root, edge, and
escape evidence is checked independently for `main`, global initialization, direct calls,
construction, virtual dispatch, and address taking. The corpus run is therefore fully reviewed: all
18 findings are true positives, no live expectation is reported, and no deliberately unreachable
expectation is missing.

The scope-separation fixture indexes a framework implementation without making its four symbols
reportable. Reachability crosses `Application::run`, returns through virtual dispatch to the
application callback, and retains its helper. A separate application helper remains the sole
finding, while one referenced declaration outside the workspace is retained as an opaque terminal.

## Reproduction

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
./build/tests/cxx_dead_golden_tests
./build/cxx-dead tests/fixtures/golden/compile_commands.json \
  --project-root tests/fixtures/golden \
  --format json
```

Wall time and peak RSS naturally vary. Commit updated measurements only when the tool, fixture,
compiler, or measurement method changes; ordinary rerun noise is not a result change.
