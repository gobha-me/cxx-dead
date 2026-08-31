# Coding-agent workflow

This is the supported routine coding-agent check for one changed C++ target. It compares a complete,
target-scoped current graph with a compatible graph from the base revision and asks an agent to
investigate only newly introduced `dead` or `likely_dead` unreachable implementation. It does not
detect semantic clones and never authorizes automatic deletion.

## Copy-ready prompt fragment

> After building and testing the changed C++ target, run `cxx-dead` against that target's CMake File
> API metadata and compare it with a graph generated from the base revision. Use the same frontend,
> configuration ID, selected target, report paths, provider configuration, and resource limits for
> both runs. Investigate each policy-matching `new_symbol` or `newly_unreachable` finding in source
> and call-site context. A match may be intentional staged work; do not automatically delete it and
> do not describe reachability analysis as semantic clone detection. Treat an incomplete,
> unsupported, incompatible, cancelled, or otherwise failed run as analysis unavailable, never as a
> clean result. If trustworthy target metadata is unavailable, report that the check could not run
> instead of widening analysis to the whole compilation database.

## Baseline and current setup

Run the baseline and current revisions from the same task-owned scratch source path so project-relative
locations, target identity, and cache inputs remain comparable. Checkout and build orchestration are
external to `cxx-dead`; the tool never changes Git state or builds a revision implicitly. Do not move
the user's active checkout between revisions.

Before configuring each revision, request the CMake codemodel and export compile commands, then build
the selected target. Build parallelism of two is the resource-conscious default:

```bash
cmake -E make_directory <build-dir>/.cmake/api/v1/query
cmake -E touch <build-dir>/.cmake/api/v1/query/codemodel-v2
cmake -S <source-dir> -B <build-dir> \
  -DCMAKE_BUILD_TYPE=<configuration> \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build <build-dir> --target <target> --parallel 2
```

Run the project's relevant tests before analysis. Reconfigure and rebuild after changing the scratch
checkout from the base revision to the current revision. Keep all graphs, reports, caches, and build
trees inside the task-owned scratch directory and remove only those paths when finished.

Use this schema-2 symbolic policy for the initial adoption gate:

```yaml
schema_version: 2
changes: [new_symbol, newly_unreachable]
classifications: [dead, likely_dead]
```

Generate the baseline graph at the base revision:

```bash
cxx-dead \
  --cmake-build-dir <build-dir> \
  --configuration <configuration> \
  --target <target> \
  --configuration-id <stable-configuration-id> \
  --report-path <source-dir>/include \
  --report-path <source-dir>/src \
  --tu-timeout 120 \
  --index-timeout 900 \
  --max-ast-bytes 2147483648 \
  --cache-dir <scratch-dir>/cache \
  --verbose \
  --format json \
  --output <scratch-dir>/baseline.report.json \
  --graph-output <scratch-dir>/baseline.graph.json
```

At the current revision, repeat the same target, configuration, identity, paths, frontend, providers,
and limits, then compare with the baseline:

```bash
cxx-dead \
  --cmake-build-dir <build-dir> \
  --configuration <configuration> \
  --target <target> \
  --configuration-id <stable-configuration-id> \
  --report-path <source-dir>/include \
  --report-path <source-dir>/src \
  --tu-timeout 120 \
  --index-timeout 900 \
  --max-ast-bytes 2147483648 \
  --cache-dir <scratch-dir>/cache \
  --verbose \
  --baseline-graph <scratch-dir>/baseline.graph.json \
  --diff-policy <scratch-dir>/cxx-dead-diff.yaml \
  --fail-on-diff \
  --format json \
  --output <scratch-dir>/current.diff.json \
  --graph-output <scratch-dir>/current.graph.json
```

Replace the example `--report-path` values with every project-owned subtree that should produce
findings. Code elsewhere under the CMake source root can still participate in reachability. Keep the
same explicit provider files on both commands when the target depends on dynamic roots, callbacks,
or suppressions. Use `--format sarif` for annotation upload only when the JSON investigation report
is not required from that invocation.

## Result handling

| Status | Meaning | Agent action |
| ---: | --- | --- |
| `0` | Complete comparison with no policy matches | Record the policy gate as clean; do not claim that no unreachable code exists. |
| `2` | Complete comparison with one or more policy matches | Inspect each match and its callers; never delete automatically, and change code only with corroborating source evidence. |
| `1` | Invalid setup, incompatible baseline, indexing/resource failure, or unsupported request | Report analysis unavailable and preserve the diagnostic; never infer zero findings. |
| `130` / `143` | Cancelled by `SIGINT` / `SIGTERM` | Report cancellation or perform one justified bounded retry. |

The default AST JSON frontend indexes translation units sequentially. The example 120-second,
900-second, and 2 GiB AST-output bounds are the envelope validated by the
[Obscura rollout](obscura-trial.md), not a universal performance guarantee. A project may adopt lower
measured bounds. Raise a limit only from diagnostic evidence and available machine capacity; do not
loop retries or remove bounds to obtain a passing result. Cache reuse is expected to make unchanged
or warm follow-up runs inexpensive.

## Fail-closed fallbacks

- If CMake File API metadata is unavailable, use `--target-manifest` only when a trusted build adapter
  can supply the documented [schema-2 target and dependency closure](target-model.md). Otherwise
  report the check as unavailable; do not run an unscoped compilation database as an equivalent
  gate.
- If the baseline is absent, incomplete, malformed, schema-incompatible, or from a different
  frontend, configuration identity, or target, regenerate it from the exact base revision. Do not
  compare against a convenient but incompatible artifact.
- If indexing is incomplete, preserve its structured diagnostic and the absence of a graph. A warm
  cache or one evidence-based resource adjustment may be retried when the shared environment allows;
  otherwise stop and report the unavailable check.
- Do not switch to LibTooling to bypass hard limits. The in-process frontend reports bounded AST
  limits as unsupported rather than silently approximating them.
