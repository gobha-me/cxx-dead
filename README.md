# cxx-dead

`cxx-dead` is an experimental, context-aware whole-program C++ reachability analyzer. The current prototype answers one deliberately narrow question:

> For the translation units in this compilation database, which project-owned function definitions have no path from `main()` or a configured root?

It uses Clang to extract declarations and references, constructs a typed symbol graph, traverses that graph from application roots, identifies unreachable strongly connected components, and emits human-readable or JSON findings.

The prototype already handles:

- direct calls across translation units;
- methods, constructors, and destructors;
- constructor/destructor effects for base classes;
- conservative virtual dispatch across known class hierarchies;
- address-taken functions as uncertain dynamic references;
- `main()`, global-initializer calls, and manual roots;
- an experimental Clang declaration-name filter for namespace-scoped trials;
- unreachable cycles using Tarjan's SCC algorithm;
- aggregation when every defined member of a type is unreachable;
- internal-linkage confidence evidence;
- human and versioned JSON output.

It does not yet reconstruct linker targets, infer library APIs, model arbitrary registration/plugin systems, analyze multiple build configurations, or scale efficiently to large standard-library-heavy projects. Findings are candidates for review, not deletion instructions.

## Development status

`cxx-dead` is a pre-alpha research prototype. Its CLI, JSON schema, and analysis semantics may
change between revisions. It is useful for experiments and reviewed audits, but it is not yet a
safe basis for automatic source deletion or blocking production CI.

The current implementation is Linux/POSIX-only because the Clang subprocess runner uses POSIX
process APIs. It has been exercised with GCC 14, Clang 20, and Clang 20 AST output.

## Build

Requirements are a C++23 compiler, CMake 3.25 or later, and a Clang executable capable of `-ast-dump=json` (tested with Clang 20).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

No LLVM development package or third-party JSON library is required by this prototype.

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

Clang remains a runtime requirement: `cxx-dead` invokes `clang++` from `PATH` by default, or the
executable supplied with `--clang`.

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

JSON output for automation:

```bash
cxx-dead build/compile_commands.json \
  --project-root . \
  --format json \
  --output cxx-dead.json
```

Additional roots and CI behavior:

```bash
cxx-dead build/compile_commands.json \
  --root engine::plugin_entry \
  --fail-on-unreachable
```

If a dependency-heavy compilation database contains translation units outside the application, scope
the input without rewriting the database:

```bash
cxx-dead build/compile_commands.json \
  --tu-root src \
  --exclude-path build/_deps
```

`--fail-on-unreachable` returns status 2 when any unreachable candidate is present. Analysis/indexing errors return status 1. A successful advisory run returns status 0 regardless of findings.

Run `cxx-dead --help` for all current options.

For a codebase consistently contained in one namespace, Clang can avoid dumping most system
declarations. Because the filter can exclude `main`, explicitly provide the application function it
calls:

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
- `dynamically_referenced`: unreachable by call edges, but its address is taken.

The numeric confidence values in JSON are provisional presentation values, not statistically calibrated probabilities. CI should initially filter by classification rather than treating them as measured likelihoods.

## Current analysis contract

The compilation database is treated as one application and every listed translation unit is assumed to be linked into it. This assumption is intentionally explicit: `compile_commands.json` describes compilation, not link membership or target relationships. For meaningful results, pass a database restricted to one executable target.

Clang is invoked directly with normalized compile arguments and without a shell. Output/dependency-generation flags are removed, and AST JSON is captured in memory. Repository-provided compiler plugins or response files remain part of the trusted build input.

Only declarations below `--project-root` are placed in the prototype graph. Calls into system and
third-party code terminate at that boundary. This reduces AST graph pollution but means callbacks or
construction performed inside library templates require explicit modeling.

## Design documents

- [Design review](docs/design-review.md) critiques the proposal and records decisions made for the prototype.
- [Implementation plan](docs/implementation-plan.md) turns the proposal into phased, testable milestones.
- [Architecture](docs/architecture.md) describes the current code and its replacement boundaries.
- [TermForge field trial](docs/termforge-trial.md) records real-project measurements, bugs found, and
  finding interpretation.
- [Null Vector field trial](docs/null-vector-trial.md) covers dependency scoping, source mapping,
  namespace aliases, and framework callback limitations.
- [Roadmap](ROADMAP.md) defines the maintained milestones and their acceptance gates.

## Prototype fixture

The fixture in `tests/fixtures/application` intentionally contains live cross-TU calls, a dead cycle, an abandoned type, an escaped callback, virtual dispatch, and an internal dead function. It can be inspected manually with:

```bash
./build/cxx-dead tests/fixtures/application/compile_commands.json \
  --project-root tests/fixtures/application
```

## Contributing and license

Bug reports—especially minimized false positives—are valuable at this stage. See
[CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request.

`cxx-dead` is distributed under the [BSD 3-Clause License](LICENSE).
