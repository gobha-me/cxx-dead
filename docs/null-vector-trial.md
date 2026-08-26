# Null Vector field trial

Date: 2026-08-24

## Scope

Null Vector is a small application with nine translation units, public headers under `include/`,
application support under `src/bin`, a static library under `src/lib`, and a pinned TermForge
dependency supplied through FetchContent.

The final run used the pinned dependency's compilation database while selecting only Null Vector
translation units and excluding fetched build-tree declarations from the project graph:

```bash
cxx-dead ../null-vector/build-cxx-dead-pinned/compile_commands.json \
  --tu-root ../null-vector/src \
  --project-root ../null-vector \
  --exclude-path ../null-vector/build \
  --format json
```

The unfiltered run preserved global `main`, the anonymous-namespace `NullVectorApp`, and code paths
that a namespace-only filter would omit.

## Configuration failure caught

An initial configuration resolved the current local TermForge build instead of Null Vector's pinned
v0.9.4 dependency. Clang rejected `main.cpp` because `Cell` and `Screen::clear` APIs did not match.
`cxx-dead` failed the entire run and emitted no partial report, validating the index-completeness
policy. Reconfiguration against the already-fetched v0.9.4 source fixed the compilation context.

The fetched project contributed 33 dependency commands to the database. This prompted the addition
of `--tu-root` for command selection and repeatable `--exclude-path` for declaration ownership.

## Measurements

The final nine-TU run completed in 2 minutes 25.4 seconds and peaked around 3.6 GiB resident memory
on the large `main.cpp` AST.

```text
defined project symbols       123
reachable                      79
unreachable candidates         44

dead/internal                  12
likely_dead                    25
possibly_dead                   7
```

The pre-fix report contained 531 supposed project definitions, including hundreds of `std::*`,
`__gnu_cxx::*`, and `__gthread_*` helpers. The final report contains zero symbols with those prefixes.

## Bugs found in cxx-dead

### Header location inheritance

Some GCC compatibility-header nodes omit `loc.file` while retaining `includedFrom`. The indexer
inherited the translation-unit filename and misattributed those header functions to Null Vector.
The location fallback now treats such top-level nodes as unknown/external instead of project source.

### Aliased construction

Constructor matching used Clang's written `qualType`. Expressions written through the `nv` namespace
alias therefore failed to match definitions named under `null_vector`, incorrectly reporting
`GameState`, `GameCanvas`, `Framebuffer`, and `PauseOverlay` constructors. Construction now prefers
`desugaredQualType`; a dedicated alias-construction fixture covers the behavior.

The correction increased reachable symbols from 75 to 79 and reduced candidates from 48 to 44.

## Interpretation

The remaining 44 findings are not deletion evidence.

The largest coherent group begins with TermForge framework callbacks:

```text
NullVectorApp::on_start
NullVectorApp::on_event
NullVectorApp::on_tick
NullVectorApp::on_render
NullVectorApp::on_pixels
```

TermForge owns the calls to those virtual methods. Its declarations and implementation were excluded
from the project graph, so callback reachability cannot flow back into Null Vector. That makes the
following directly-called helpers appear dead as a cascade: `action_for`, `apply_key`, `draw_title`,
`draw_result`, `draw_tactical`, `direction_to`, `phase_name`, and several state/canvas methods.

Other candidates are intentionally consumed by tests or are small public conveniences, including
`GameState::set_player_pose`, equality operators, and canvas inspection methods.

No production Null Vector symbol was established as safely removable.

## Architectural conclusion

Dependency exclusion cannot serve both graph scope and reporting scope. A framework implementation
must be allowed to participate in virtual/callback reachability without becoming a report candidate.
The next graph model should therefore distinguish at least:

```text
indexed for reachability
owned/reportable
external opaque
excluded entirely
```

This field trial makes graph-scope/report-scope separation a prerequisite for trustworthy framework
application analysis.

## Scope-model follow-up

Version 0.3.0 separates these policies. `--project-root` now bounds the indexed workspace, while
repeatable `--report-path` values select owned definitions eligible for findings. For a future Null
Vector rerun, use `include/` and `src/` as report paths while retaining the linked TermForge sources
inside the indexed workspace. The reduced framework fixture covers the callback-to-application
virtual-dispatch cascade; a target-aware rerun remains dependent on explicit link membership.

## v0.5.0 frontend comparison

The direct frontend was compared with AST JSON on Null Vector revision `0786d15`, selecting the same
nine source translation units and excluding the build tree.

| Frontend | Wall time | Peak RSS | AST bytes | Fact bytes | Defined / graph symbols | Edges | Findings |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| AST JSON | 128.1 s | 3.52 GiB | 5,025,849,020 | 680,475 | 123 / 642 | 470 | 44 |
| LibTooling | 8.0 s | 225.6 MiB | 0 | 1,477,703 | 123 / 1,306 | 882 | 44 |

LibTooling reduced wall time by about 94% and peak RSS by about 94%, while retaining the same 123
definitions, 79 reachable definitions, and 44 findings. It materializes more referenced external
terminals and edges directly from Clang's AST, explaining the larger neutral fact payload without a
reportable-result change. The known framework callback cascade remains advisory.
