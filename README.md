# cxx-dead

`cxx-dead` is an experimental, context-aware whole-program C++ reachability analyzer. The current prototype answers one deliberately narrow question:

> For the translation units in this compilation database, which project-owned function definitions have no path from `main()` or a configured root?

It uses Clang to extract declarations and references, constructs a typed symbol graph, traverses that graph from application roots, identifies unreachable strongly connected components, and emits human-readable or JSON findings.

The prototype already handles:

- direct calls across translation units;
- methods, constructors, and destructors;
- selected direct and alias construction with base/member initialization and cleanup effects;
- conservative `std::make_unique`/`std::make_shared` construction and diagnosed owning-pointer
  factory fallbacks;
- conservative virtual dispatch across known class hierarchies;
- address-taken functions and callable objects as provider-attributed escape evidence;
- configurable callback-argument registration with structural/provider reachability provenance;
- strict YAML providers for explicit roots, dynamic edges, escapes, and auditable suppressions;
- `main()`, global-initializer calls, and manual roots;
- an experimental declaration-name filter for namespace-scoped trials;
- unreachable cycles using Tarjan's SCC algorithm;
- aggregation when every defined member of a type is unreachable;
- internal-linkage confidence evidence;
- structured root, graph-edge, escape, and classification evidence;
- separate reachability and reporting scopes for framework-aware analysis;
- optional direct LibTooling fact extraction without full JSON AST materialization;
- configuration-aware stable symbol identities and deterministic graph artifacts;
- target-scoped analysis from CMake File API or explicit manifest metadata;
- conservative shared/static library public-API rooting from source metadata;
- bounded AST JSON indexing with per-TU/run timeouts and output-size limits;
- structured complete, incomplete, and unsupported run diagnostics;
- human and versioned JSON output;
- complete display signatures and exact spelling/expansion source extents.

It does not inspect built binary export tables, parse linker version scripts, exactly model
static-archive member extraction, model non-owning factory semantics, jointly analyze multiple
build configurations in one report, or incrementally cache translation-unit facts.
Findings are candidates for review, not deletion instructions.

## Development status

`cxx-dead` is a pre-alpha research prototype. Its CLI, JSON schema, and analysis semantics may
change between revisions. It is useful for experiments and reviewed audits, but it is not yet a
safe basis for automatic source deletion or blocking production CI.

The current implementation is Linux/POSIX-only because the AST JSON frontend uses POSIX process
APIs. It has been exercised with GCC 14, Clang 18/20, and Clang 20 AST output.

## Build

The default build requires a C++23 compiler, CMake 3.25 or later, `yaml-cpp`, and a Clang executable
capable of `-ast-dump=json` (tested with Clang 20).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

No LLVM development package or third-party JSON library is required for the default AST JSON
frontend. The YAML provider uses the required `yaml-cpp` package. To build the experimental
LibTooling frontend, install matching LLVM/Clang development packages and configure their CMake
package directories when they are not on the default path:

```bash
cmake -S . -B build-libtooling \
  -DCMAKE_BUILD_TYPE=Release \
  -DCXX_DEAD_ENABLE_LIBTOOLING=ON \
  -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm-20/lib/cmake/clang
cmake --build build-libtooling -j
ctest --test-dir build-libtooling --output-on-failure
```

The optional build links `clang-cpp`; the default remains dependency-light and selects AST JSON.

## Install

The portable CMake install command installs to the configured system prefix:

```bash
cmake --install build
```

With a Makefiles generator, `make -C build install` invokes the same install rules. To install for
the current user without elevated privileges, override the prefix at install time:

```bash
cmake --install build --prefix "$HOME/.local"
```

This places the executable at `$HOME/.local/bin/cxx-dead`. Ensure `$HOME/.local/bin` is in `PATH`.
An equivalent persistent configuration is:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel 2
cmake --build build --target install
```

The default AST JSON frontend requires Clang at runtime: `cxx-dead` invokes `clang++` from `PATH` by
default, or the executable supplied with `--clang`. The optional LibTooling frontend uses the
LLVM/Clang libraries linked into the binary instead of launching a compiler executable.

## Run

Generate a compilation database for the application configuration under analysis. With CMake:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Then run:

```bash
cxx-dead \
  --compile-commands build/compile_commands.json \
  --project-root . \
  --mode application
```

For a build containing more than one executable or library, request CMake's codemodel before
configuring and select one target explicitly:

```bash
cmake -E make_directory build/.cmake/api/v1/query
cmake -E touch build/.cmake/api/v1/query/codemodel-v2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cxx-dead \
  --cmake-build-dir build \
  --configuration Debug \
  --target my_app \
  --format json
```

The selected executable and its transitive object, static, shared, and module-library dependencies
are analyzed without indexing sibling executables or tests. If the codemodel has one configuration
and one executable, both selections are automatic. Ambiguous configurations or targets fail with
an instruction to select one; the tool never silently combines them. See the
[target model](docs/target-model.md) for the manifest fallback and link-semantics contract.

Selecting a shared or module library roots external definitions with effective default/protected
Clang visibility and external declarations observed in its CMake PUBLIC/INTERFACE header file sets.
Selecting a static library fails closed unless such public headers or explicit public API roots are
declared. Manifest schema 2 supplies exact `public_headers` paths for non-CMake build adapters.

An enabled LibTooling build can select direct fact extraction explicitly:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --frontend libtooling
```

`--frontend ast-json` remains the default. `--clang` applies only to that subprocess frontend.
With `--verbose`, either frontend writes one `cxx-dead-index-metrics` line to standard error with
translation-unit, AST/fact-byte, wall-time, peak-RSS, symbol, and edge counters.

JSON output for automation:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --format json \
  --output cxx-dead.json
```

Bound the dependency-light AST JSON frontend for routine or untrusted-duration runs:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --tu-timeout 120 \
  --index-timeout 900 \
  --max-ast-bytes 536870912 \
  --format json
```

The limits are opt-in and use whole seconds and bytes. Exceeding any limit discards all graph facts,
emits an `incomplete` run document without a `findings` field, and returns status 1. `SIGINT` and
`SIGTERM` terminate the active Clang process group before returning a cancelled diagnostic. Hard
subprocess limits are not silently approximated by the in-process LibTooling frontend: combining
them with `--frontend libtooling` returns a structured `unsupported` run.

Choose limits from a representative unrestricted run rather than treating the example as a policy:
allow headroom above the slowest translation unit and total indexing time, and remember that a
template-heavy AST JSON document may require hundreds of MiB or more. For routine automation, a
limit failure is an analysis failure to investigate or reconfigure, never a passing no-findings run.

Write a deterministic, independently versioned graph artifact for later comparison or indexing
work. Give each separately analyzed build configuration a durable identifier:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --configuration-id linux-debug \
  --format json \
  --output cxx-dead.json \
  --graph-output cxx-dead.graph.json
```

`--configuration-id` defaults to `default`. It participates in every stable symbol ID, so use the
same value for comparable runs and distinct values for configurations whose declarations may
differ. The graph artifact records symbols, typed edges, roots, escapes, source extents, identity
quality, and diagnostics. Artifact ingestion and cache reuse are not implemented yet.

Additional roots and CI behavior:

```bash
cxx-dead build/compile_commands.json \
  --root engine::plugin_entry \
  --fail-on-unreachable
```

Register a callback argument conservatively when a known registrar executes. The index is
zero-based, the option is repeatable, and `CALLEE` must exactly match a qualified name, linkage
name, or stable symbol ID:

```bash
cxx-dead build/compile_commands.json \
  --callback-registration engine::register_handler:1 \
  --format json
```

Ordinary callable storage or passage records non-traversing escape evidence. Only a matched
registration rule creates provider reachability, and a registration call in unreachable code does
not retain its callback. Malformed, unmatched, out-of-range, or non-callable rules fail the run.

For general project policy, load one or more explicit YAML providers:

```yaml
schema_version: 1
provider: project_policy
roots:
  - symbol: {qualified_name: engine::plugin_entry}
    reason: loaded by the runtime plugin manager
edges:
  - from: {qualified_name: engine::plugin_entry}
    to: {qualified_name: engine::dispatch}
    reason: runtime plugin dispatch
escapes:
  - symbol: {linkage_name: _ZN6engine8callbackEv}
    reason: stored by an external runtime
suppressions:
  - symbol: {qualified_name: engine::legacy_hook, signature: "void ()"}
    reason: retained for deployed plugin compatibility
callback_registrations:
  - callee: {qualified_name: engine::register_handler}
    argument_index: 1
    reason: registrar owns callback argument one
```

```bash
cxx-dead build/compile_commands.json \
  --provider-config cxx-dead.yaml \
  --format json
```

Each selector sets exactly one of `id`, `linkage_name`, or `qualified_name`; a signature may refine
a qualified name. Provider selectors must resolve to exactly one graph symbol. Unknown fields,
duplicate keys, unsupported schema versions, unmatched selectors, and ambiguous selectors fail the
run. Files compose additively and order-independently; there is no implicit discovery, environment
expansion, globbing, or regex matching.

Provider schema 2 additionally distinguishes library API roots from runtime/provider roots:

```yaml
schema_version: 2
provider: sdk_contract
public_api_roots:
  - symbol: {qualified_name: engine::sdk_entry}
    reason: supported external SDK entry point
```

`public_api_roots` require target-aware analysis of a selected static, shared, module, or interface
library. Provider schema 1 remains accepted unchanged. Public API roots appear separately as
`externally_reachable`; their transitive implementation is counted as internal live code.

If a dependency-heavy compilation database contains translation units outside the application, scope
the input without rewriting the database:

```bash
cxx-dead build/compile_commands.json \
  --tu-root src \
  --exclude-path build/_deps
```

To retain framework code in reachability while reporting only application-owned paths, set the
workspace boundary with `--project-root` and repeat `--report-path` for each owned subtree:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --report-path include \
  --report-path src
```

Definitions elsewhere under the project root are indexed but never reported. Referenced declarations
outside the project root are opaque graph terminals, and `--exclude-path` removes matching
declarations entirely.

`--fail-on-unreachable` returns status 2 when a complete run contains any actionable unreachable
candidate. Provider-suppressed candidates remain in `suppressed_findings` with their original
classification and suppression provenance, but do not trigger status 2.
Analysis/indexing errors, resource limits, and unsupported requests return status 1. A successful
advisory run returns status 0 regardless of findings. Signal cancellation remains fail-closed and
uses the conventional signal-derived status (130 for `SIGINT`, 143 for `SIGTERM`).

Run `cxx-dead --help` for all current options.

For a codebase consistently contained in one namespace, either frontend can avoid materializing
most system declaration facts. Because the filter can exclude `main`, explicitly provide the
application function it calls:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --ast-filter my_project \
  --root my_project::run
```

This is an experimental scalability aid, not a general source-path filter. The report includes a
diagnostic whenever it is active, and analysis fails if no application root remains.

## Interpretation

The prototype assigns one of these evidence-based classifications:

- `dead`: unreachable internal-linkage definition with no observed escape;
- `likely_dead`: unreachable application definition with no observed escape;
- `possibly_dead`: unreachable virtual definition requiring conservative review;
- `dynamically_referenced`: unreachable by traversable edges, but its address or callable object
  escapes.

The numeric confidence values in JSON are provisional presentation values, not statistically calibrated probabilities. CI should initially filter by classification rather than treating them as measured likelihoods.

Every classification is backed by an ordered evidence chain. Root, edge, and escape facts retain a
provider and human-readable reason, while analysis decisions use typed facts rather than matching
those presentation strings. JSON report schema version 10 adds separately evidenced public API,
internal-live, and internal-unreachable counts; version 9 added actionable/suppressed counts and
auditable `suppressed_findings`; version 8 added structural/provider reachability counts and
provider-retained callback evidence; version 7 added explicit run state and
per-translation-unit diagnostics, and version 6 added an application/target analysis context.
Version 5 changed `key` to a configuration-aware stable symbol ID. Version 4 added
complete signatures and spelling/expansion locations and definition ranges while retaining the flat
finding `file` and `line` fields. Graph artifact schema version 5 adds public API roots; version 4
added provider suppressions and general configured roots, edges, and escapes; version 3 added callback-registration edges and
callable-object escapes; version 2 added target context.
Identity schema version 1 is unchanged and remains independent from the report schema.

## Current analysis contract

Without build metadata, the compilation database is treated as one application and every listed
translation unit is assumed to be linked into it. With `--cmake-build-dir` or `--target-manifest`,
only the selected target's transitive link closure is indexed. Target mode records the selected
configuration, target id/name/kind, and closure in human, JSON, and graph-artifact output.

AST JSON invokes Clang directly with normalized compile arguments and without a shell. Each Clang
invocation runs in a dedicated process group so timeout, output-limit, or cancellation handling also
terminates descendants. LibTooling
runs an in-process `FrontendAction` over the same compilation commands. Both remove output and
dependency-generation flags and merge one neutral graph fact batch per translation unit.
Repository-provided compiler plugins or response files remain part of the trusted build input.

Definitions below `--project-root` participate in reachability. When `--report-path` is omitted, the
whole project root is reportable for backward compatibility; otherwise only definitions below an
explicit report path can become findings. Referenced declarations outside the project root are
materialized lazily as opaque terminals, and excluded paths do not enter the graph.

## Design documents

- [Design review](docs/design-review.md) critiques the proposal and records decisions made for the prototype.
- [Implementation plan](docs/implementation-plan.md) turns the proposal into phased, testable milestones.
- [Architecture](docs/architecture.md) describes the current code and its replacement boundaries.
- [Target model](docs/target-model.md) defines CMake ingestion, manifest fallback, and target-kind
  semantics.
- [LibTooling frontend decision](docs/adr/0001-libtooling-frontend.md) records the measured frontend
  choice and compatibility tradeoffs.
- [TermForge field trial](docs/termforge-trial.md) records real-project measurements, bugs found, and
  finding interpretation.
- [Null Vector field trial](docs/null-vector-trial.md) covers dependency scoping, source mapping,
  namespace aliases, and framework callback limitations.
- [Result ledger](docs/result-ledger.md) records reviewed corpus outcomes, resource measurements,
  and the completeness of each measurement.
- [Roadmap](ROADMAP.md) defines the maintained milestones and their acceptance gates.

## Prototype fixture

The fixture in `tests/fixtures/application` intentionally contains live cross-TU calls, a dead cycle, an abandoned type, an escaped callback, virtual dispatch, and an internal dead function. It can be inspected manually with:

```bash
./build/cxx-dead tests/fixtures/application/compile_commands.json \
  --project-root tests/fixtures/application
```

`tests/fixtures/construction` separately covers selected overloads, aliases, base/member lifetime
effects, standard smart-pointer factories, a diagnosed custom owning-pointer factory, and negative
controls for nested or borrowed owning pointers.

## Contributing and license

Bug reports—especially minimized false positives—are valuable at this stage. See
[CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request.

`cxx-dead` is distributed under the [BSD 3-Clause License](LICENSE).
