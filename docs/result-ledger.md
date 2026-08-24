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

Measurements are machine- and configuration-specific. Peak RSS is the maximum resident set for the
analyzer process; AST bytes are the sum of Clang JSON written to stdout for successfully indexed
translation units. `Graph symbols` includes declarations as well as reportable definitions.

| Run | Revision / environment | State | Reviewed findings | TP | FP | Known FN | Wall time | Peak RSS | AST bytes | Defined / graph symbols | Edges |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Golden application corpus | v0.2.0 candidate; Clang 20.1.8; Linux x86-64 | complete | 17 / 17 | 17 | 0 | 0 | 108 ms | 4,928 KiB | 171,431 | 38 / 39 | 22 |
| TermForge `forge-top` | v0.1.0-era corrected prototype; filtered Clang run | complete | 585 / 585 triaged by category | not individually enumerated | at least 2 named factory-construction cases plus callable cases | not measured | 69.96 s | approximately 3–3.5 GiB | not captured | 1,618 / not captured | not captured |
| Null Vector application | v0.1.0-era corrected prototype; 9 selected TUs | complete | 44 / 44 triaged by category | not individually enumerated | at least 5 framework callbacks plus their live cascade | not measured | 145.4 s | approximately 3.6 GiB | not captured | 123 / not captured | not captured |
| Invalid golden translation unit | v0.2.0 candidate; Clang 20.1.8; Linux x86-64 | incomplete | 0 | 0 | 0 | 0 | not benchmarked | not benchmarked | 0 accepted | 0 committed | 0 committed |

The historical field trials predate AST-byte and edge counters. Their missing measurements remain
explicit rather than reconstructed from a different revision. Future reruns should populate every
column and retain the old row when the frontend or run configuration changes.

## Golden review coverage

The three-translation-unit corpus contains 38 project definitions and 39 graph symbols. Its 38
table-driven expectations cover both live and unreachable outcomes for:

- direct and cross-translation-unit calls, overloads, namespaces, external and internal linkage;
- constructors, destructors, base/member initialization, virtual dispatch, and unused overrides;
- directly called, address-escaped, and unused callbacks;
- used and unused template specializations;
- global initialization, macro-defined functions, generated sources, and excluded generated paths;
- merged external-linkage header definitions and fail-closed invalid translation units.

Every unreachable expectation asserts its classification and typed evidence chain. Root, edge, and
escape evidence is checked independently for `main`, global initialization, direct calls,
construction, virtual dispatch, and address taking. The corpus run is therefore fully reviewed: all
17 findings are true positives, no live expectation is reported, and no deliberately unreachable
expectation is missing.

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
