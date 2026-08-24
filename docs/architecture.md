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
- `indexer` converts Clang AST JSON into declarations, provider-attributed calls,
  construction/destruction edges, address escapes, roots, record inheritance, and conservative
  override edges. It assigns reportable, indexed, external-opaque, or excluded scope from configured
  path boundaries and materializes opaque references lazily.
- `graph` merges symbols, stores typed roots, edges, and escapes with structured evidence, traverses
  live edges, and computes unreachable SCCs.
- `report` applies typed evidence classifications and writes terminal or schema-versioned JSON
  evidence chains.
- `json` is a small standards-oriented parser/escaper used for both Clang and compilation database input, avoiding a prototype package dependency.

The graph module has no Clang dependency. A scalable frontend can replace `ClangAstIndexer` while retaining analysis and reporting tests.

## Symbol scope

`--project-root` bounds definitions that may participate in reachability. Repeatable
`--report-path` values select owned definitions eligible for findings and default to the project
root. Remaining project definitions are indexed for traversal without becoming candidates.
Referenced declarations outside the project root become opaque terminal symbols only when used;
`--exclude-path` takes precedence and omits declarations entirely. Merged cross-TU symbols retain
the strongest observed scope: reportable, then indexed, then external opaque.

## Reachability semantics

Traversed edges:

- `DirectCall`
- `Constructs`
- `VirtualDispatch`

Non-traversed escape facts:

- `AddressTaken`

Current roots:

- a defined function named `main`;
- targets called by detected namespace-scope initializers;
- exact qualified or mangled names passed with `--root`.

Each root records its typed kind, provider, and reason. Traversable edges retain equivalent provider
evidence, while address-taking is stored separately as an escape fact with its originating symbol
when known. Classification uses these enums and relationships; reasons are presentation metadata.

An experimental `--ast-filter` passes Clang's declaration-name filter through to the AST dumper.
Filtered output can contain multiple consecutive JSON documents; the indexer splits and merges them
per translation unit. Since filtering can exclude `main`, a matching manual root is required unless
another root remains.

## Identity

External functions currently merge by mangled name. Internal-linkage symbols add the translation-unit path. This is adequate for the fixture but not the final identity design; see the design review for USR/configuration requirements.

## Failure model

The index is all-or-nothing. A nonzero Clang result or malformed AST aborts analysis with exit status 1. This avoids presenting an incomplete graph as evidence of deadness.

## Run states

The prototype uses these run-state definitions even though successful JSON reports do not yet carry
an explicit state field:

- **complete**: every selected translation unit produced parseable AST facts and at least one
  application root was found. Only a complete run may emit findings or return the policy status 2.
- **incomplete**: a supported analysis started, but a required translation unit, AST document, or
  root could not be indexed. The partial graph is discarded, no findings are emitted, and the
  process returns status 1.
- **unsupported**: the requested analysis context is not implemented, such as a mode other than
  `application`. The request is rejected before a report is produced and returns status 1.

An unsupported language or framework pattern inside an otherwise complete application run is an
analysis limitation, not a different run state. Such findings remain advisory and the limitation
must be recorded in the result ledger or a diagnostic until the frontend can model it
conservatively. Structured run-state output and resource-limit behavior remain follow-up work.

## Security boundary

Commands are passed as argument arrays, not shell strings. Output and dependency flags are removed before adding AST flags. As with normal builds, the compilation database is trusted input: response files, compiler plugins, and paths can still cause Clang to load repository-selected content.
