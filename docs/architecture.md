# Prototype architecture

```text
compile_commands.json
        |
        +---- build model (optional CMake/manifest) ----> target command selection
        |                                                |
        v                                                v
command normalization ---------------------------> AST JSON subprocess ----+
        |                                            |
        +----------------> LibTooling action --------+
                                                     |
                                                     v
                                           per-TU neutral facts
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
                    +-------------+-------------+-------------+
                    |                           |             |
                    v                           v             v
              root traversal               Tarjan SCC   graph artifact
                    |                           |
                    +-------------+-------------+
                                  |
                                  v
                       classification/reporting
```

## Modules

- `compile_database` parses both `arguments` and shell-quoted `command` entries.
- `build_model` parses CMake File API codemodel-v2 replies or schema-versioned explicit manifests,
  resolves a selected target's link closure, and maps its C++ sources back to compile commands.
- `process` runs Clang directly with `fork`/`execvp`, captures stdout/stderr concurrently, and never evaluates compile commands through a shell.
- `indexer` converts Clang AST JSON into declarations, provider-attributed calls,
  construction/destruction edges, address escapes, roots, record inheritance, and conservative
  override edges. It assigns reportable, indexed, external-opaque, or excluded scope from configured
  path boundaries, reconstructs compressed Clang source locations from byte offsets, preserves
  spelling and macro-expansion extents, and materializes opaque references lazily.
- `libtooling_indexer` runs a `FrontendAction` and `RecursiveASTVisitor` over the same compilation
  commands and emits the same neutral facts directly. It is compiled only when
  `CXX_DEAD_ENABLE_LIBTOOLING=ON`.
- `graph` merges symbols, stores typed roots, edges, and escapes with structured evidence, traverses
  live edges, computes unreachable SCCs, and canonicalizes fact ordering by stable symbol ID.
- `artifact` writes deterministic graph JSON with independently versioned artifact and identity
  schemas.
- `report` applies typed evidence classifications and writes terminal or schema-versioned JSON
  evidence chains.
- `json` is a small standards-oriented parser/escaper used for both Clang and compilation database input, avoiding a prototype package dependency.

Each translation unit is merged as a separate neutral graph fact batch, so neither frontend needs
to retain all frontend documents at once. The graph and report semantics remain Clang-independent;
the parity suite requires identical golden and scope reports from both frontends.

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

An experimental `--ast-filter` limits declaration facts in both frontends. Filtered AST JSON can
contain multiple consecutive documents; the indexer splits and merges them per translation unit.
LibTooling applies the predicate before source and identity extraction and omits filtered external
terminals. Since filtering can exclude `main`, a matching manual root is required unless another
root remains.

## Identity

Symbol IDs use a collision-safe, length-prefixed identity schema. Every ID includes the explicit
`--configuration-id` value. Emitted symbols use their linkage name as the cross-frontend anchor;
LibTooling also records the Clang USR and uses it as the anchor for template patterns and other
unmangled declarations. Internal-linkage, anonymous, lambda, and function-local entities add the
project-relative translation-unit path. External and inline definitions therefore merge across
translation units while TU-local entities do not.

The dependency-light AST JSON format does not expose Clang USRs. An unmangled AST JSON declaration
uses its qualified name, signature, project-relative source path, and byte offset as a deterministic
fallback. The symbol and run diagnostics mark that identity as `fallback`; it is never silently
presented as equivalent to a USR-backed identity. Stable IDs are invariant to translation-unit
ordering and workspace relocation when configuration IDs and project-relative paths are unchanged.
Moving a declaration or changing its configuration may intentionally change its ID.

After fact merging, symbols and all referring facts are canonicalized. Incompatible kind, qualified
name, linkage domain, configuration, USR, or translation-unit metadata sharing one stable ID aborts
the run instead of silently merging. Equivalent declarations choose source and presentation data
deterministically.

## Symbol source contract

Each graph symbol retains its complete Clang `qualType` signature and a spelling source extent. A
macro-produced definition additionally retains a separate expansion extent. Every point contains a
normalized file, one-based line and column, zero-based byte offset, and token byte length. Definition
ranges use Clang's inclusive begin/end-token convention; an end-exclusive byte position is therefore
`end.offset + end.token_length`. For report ordering and the compatibility `file`/`line` JSON fields,
the expansion extent is primary when present and spelling is primary otherwise.

Clang omits repeated file and line fields in nested JSON nodes. The indexer resolves those omissions
from traversal context and a cached per-file byte-offset line map rather than emitting line zero.
JSON report schema version 6 adds analysis configuration and target context; version 5 exposed stable
keys, while version 4 introduced spelling and expansion extents. Graph artifact schema 2 adds the
same target context. Identity schema 1 remains independently versioned.

## Failure model

The index is all-or-nothing. A nonzero subprocess result, malformed AST JSON, or failed LibTooling
action aborts analysis with exit status 1. This avoids presenting an incomplete graph as evidence
of deadness.

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
