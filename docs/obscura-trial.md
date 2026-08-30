# Obscura differential field trial

Date: 2026-08-30

## Scope

This trial compared two pinned, CI-green Obscura revisions through the target-aware CMake File API
path:

- baseline `52c0c7d9c7a0cc2ba6e24a2d0af9aee1107ca151`;
- current `a1c15d8a5ec61f079d73558164ff666326af9f9f`;
- Release target `obscura`, configuration identity `obscura-release`;
- report ownership limited to `include/`, `src/`, and `cases/`.

Both revisions were configured at the same disposable source path. The baseline populated an
explicit TU cache and graph artifact, then the checkout moved to the current revision and CMake was
rerun in place. This kept project-relative source paths, target identity, and cache keys comparable
without asking cxx-dead to mutate Git state. The current target built successfully with two jobs
before analysis. Hosted Obscura CI also passed on both exact revisions.

The selected closure contained `obscura`, `obscura_lib`, and `termforge_lib`. Obscura's executable
currently prints its version and exits; it does not yet construct the game application. Its static
library is deliberately linked and developed ahead of that wiring, making this a useful check for a
new implementation that exists but is not reachable from the shipped application root.

## Reproduction contract

The baseline and current runs used the same target and resource arguments:

```text
--cmake-build-dir <obscura>/build-cxx-dead
--configuration Release
--target obscura
--configuration-id obscura-release
--report-path <obscura>/include
--report-path <obscura>/src
--report-path <obscura>/cases
--tu-timeout 120
--index-timeout 900
--max-ast-bytes 2147483648
--cache-dir <trial>/cache
```

The baseline wrote `baseline.graph.json`. The current run supplied that artifact with the existing
schema-1 high-confidence policy and `--fail-on-diff`:

```yaml
schema_version: 1
changes: [new_symbol, newly_unreachable]
classifications: [dead, likely_dead]
minimum_confidence: 0.95
```

Graph compatibility required the same frontend, configuration identity, selected target identity,
and artifact schemas. cxx-dead performed no checkout or baseline build implicitly.

## Resource and cache measurements

Measurements are specific to this Linux x86-64 development container and Clang 20.1.8. Indexing
was sequential; the only parallel work was the preceding target build, capped at two jobs.

| Run | TUs indexed / reused | Wall time | Peak RSS | AST bytes | Fact bytes | Symbols / edges |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline, cold cache | 54 / 0 | 264.370 s | 3,488,216 KiB | 27,712,181,052 | 9,798,500 | 2,294 / 2,879 |
| Current, incremental cache | 23 / 32 | 65.411 s | 2,923,164 KiB | 6,021,565,606 | 9,884,978 | 2,314 / 2,880 |
| Current, fully warm cache | 0 / 55 | 1.053 s | 47,852 KiB | 0 | 9,884,978 | 2,314 / 2,880 |

The baseline graph was 4,890,082 bytes and the current graph was 4,923,757 bytes. A fully warm
repeat produced byte-identical current graph and differential JSON files. The incremental run
reused every TermForge TU; Obscura's new private generated-asset include path changed its library
commands and correctly invalidated those entries.

Two lower AST-output bounds demonstrated the fail-closed contract:

| Per-TU limit | Outcome |
| --- | --- |
| 512 MiB | `terminal.cpp` exceeded the limit before any TU committed; exit 1, no findings, no graph. |
| 1 GiB | Four TUs indexed before `app.cpp` exceeded the limit; partial facts were discarded, exit 1, no findings, no graph. |
| 2 GiB | Complete baseline, incremental, and warm runs; differential policy exit 2 on one match. |

The 2 GiB setting is a ceiling on accepted AST output, not expected resident memory. It must still
be selected from a representative run and kept alongside the wall-time bounds. Treating either
lower-limit failure as a passing no-findings result would have hidden an incomplete analysis.

## Differential review

The comparison reported one change and one policy match:

```text
new_symbol  obscura::render::hold_d0
classification: likely_dead
confidence: 0.95
location: src/render/art_plate.cpp:14
evidence: no path from an application root
```

This is a true positive in the selected application context. The current executable does not call
the library, and no production code calls the new baked-art accessor. It is nevertheless intentional
staged work: Obscura's tests validate the asset contract, and later application wiring is expected to
consume it. The correct agent response is to investigate and retain it, not automatically delete it.

The baseline already contained 83 other application-context unreachable candidates. Differential
policy ignored that historical inventory and isolated only the newly introduced function. Policy
SARIF contained exactly one repository-relative result at `src/render/art_plate.cpp:14`, and
`--fail-on-diff` returned 2 rather than conflating the finding with analysis failure.

## Adoption conclusion

Obscura completes the third real-application advisory rollout after TermForge and Null Vector. The
single high-confidence differential match was explainable and not a known-live false positive. The
trial supports promoting cxx-dead as an investigative agent check, provided the prompt preserves
target selection, compatible baseline artifacts, explicit resource bounds, cache reuse, distinct
exit handling, and the prohibition on automatic deletion.

The cold AST JSON path remains expensive, and static-archive member extraction is not modeled: the
selected static libraries contribute all their TUs. Those are documented scalability limitations,
not permission to weaken incomplete-run behavior. Agent-prompt adoption remains tracked separately
in issue #23.
