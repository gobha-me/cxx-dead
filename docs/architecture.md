# Prototype architecture

```text
compile_commands.json
        |
        v
command normalization ----> clang++ -ast-dump=json
                                  |
                                  v
                         per-TU AST facts
                                  |
                    +-------------+-------------+
                    |                           |
                    v                           v
              symbol identity               typed edges
                    |                           |
                    +-------------+-------------+
                                  |
                                  v
                             neutral Graph
                                  |
                    +-------------+-------------+
                    |                           |
                    v                           v
              root traversal               Tarjan SCC
                    |                           |
                    +-------------+-------------+
                                  |
                                  v
                       classification/reporting
```

## Modules

- `compile_database` parses both `arguments` and shell-quoted `command` entries.
- `process` runs Clang directly with `fork`/`execvp`, captures stdout/stderr concurrently, and never evaluates compile commands through a shell.
- `indexer` converts Clang AST JSON into declarations, calls, construction/destruction edges, address escapes, roots, record inheritance, and conservative override edges.
- `graph` merges symbols, stores typed edges, traverses live edges, and computes unreachable SCCs.
- `report` applies provisional evidence classifications and writes terminal or JSON output.
- `json` is a small standards-oriented parser/escaper used for both Clang and compilation database input, avoiding a prototype package dependency.

The graph module has no Clang dependency. A scalable frontend can replace `ClangAstIndexer` while retaining analysis and reporting tests.

## Reachability semantics

Traversed edges:

- `DirectCall`
- `Constructs`
- `VirtualDispatch`

Non-traversed evidence edges:

- `AddressTaken`

Current roots:

- a defined function named `main`;
- targets called by detected namespace-scope initializers;
- exact qualified or mangled names passed with `--root`.

An experimental `--ast-filter` passes Clang's declaration-name filter through to the AST dumper.
Filtered output can contain multiple consecutive JSON documents; the indexer splits and merges them
per translation unit. Since filtering can exclude `main`, a matching manual root is required unless
another root remains.

## Identity

External functions currently merge by mangled name. Internal-linkage symbols add the translation-unit path. This is adequate for the fixture but not the final identity design; see the design review for USR/configuration requirements.

## Failure model

The index is all-or-nothing. A nonzero Clang result or malformed AST aborts analysis with exit status 1. This avoids presenting an incomplete graph as evidence of deadness.

## Security boundary

Commands are passed as argument arrays, not shell strings. Output and dependency flags are removed before adding AST flags. As with normal builds, the compilation database is trusted input: response files, compiler plugins, and paths can still cause Clang to load repository-selected content.
