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
- `process` runs Clang directly with `fork`/`execvp`, captures stdout/stderr concurrently, applies
  wall-time/output bounds, and terminates the dedicated child process group on limit or cancellation.
  It never evaluates compile commands through a shell.
- `cache` hashes normalized frontend/tool/command inputs and compiler-reported dependencies,
  validates a versioned per-TU fact representation, and stages same-directory atomic writes for
  publication only after a complete indexing run.
- `indexer` converts Clang AST JSON into declarations, provider-attributed calls,
  construction/destruction edges, address escapes, roots, record inheritance, and conservative
  override edges. Direct construction selects the observed constructor signature; standard
  smart-pointer factories and other owning-pointer-returning helpers conservatively retain the
  element's constructors and destructor, with unsupported helper semantics diagnosed. It assigns
  reportable, indexed, external-opaque, or excluded scope from configured path boundaries,
  reconstructs compressed Clang source locations from byte offsets, preserves spelling and
  macro-expansion extents, and materializes opaque references lazily.
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
the parity suite requires identical golden, scope, and construction reports from both frontends.
The CLI enables a project-local cache by default while direct library callers opt in through
`IndexOptions`. Cached batches retain the auxiliary class-hierarchy and callback-match facts needed
for whole-index validation; cache hit/miss state is telemetry only and does not alter run JSON.

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
- `CallbackRegistration`

`Constructs` edges identify selected direct constructors, corresponding destructors, observed
base/member initialization, and conservative smart-pointer factory lifetime effects. An
unrecognized helper returning `unique_ptr<T>` or `shared_ptr<T>` retains `T` rather than emitting a
high-confidence false positive and adds a deterministic diagnostic naming the helper.

Non-traversed escape facts:

- `AddressTaken`
- `CallableObject`

Callable expressions that resolve to a unique function, member, functor, or lambda call operator
produce structural call edges. Passing or storing an otherwise ambiguous callable records escape
evidence without implying invocation; reassignment invalidates initializer-based indirect-call
resolution. Repeatable callback-registration rules add traversable
provider edges only from the actual registration call site; reports separately count structural
and provider reachability.

Repeatable `--provider-config` files produce the same typed fact model for explicit roots, dynamic
edges, escapes, suppressions, and callback-registration rules. Each file has a required provider
identity and evidence reason. Exact stable-id, linkage-name, or qualified-name selectors resolve
against the merged graph; unmatched or ambiguous selectors fail before analysis output. Provider
files are applied canonically, so command-line order does not affect graph artifacts.

Current roots:

- a defined function named `main`;
- targets called by detected namespace-scope initializers;
- exact qualified or mangled names passed with `--root`.
- exact symbols selected by a YAML provider.

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
JSON report schema version 10 adds public API, internal-live, and internal-unreachable classes;
version 9 separated actionable and provider-suppressed findings while retaining suppression
provenance; version 8 added callable/provider provenance; version 7 added run state and
translation-unit diagnostics, while version 6 added
analysis configuration and target context, version 5 exposed stable keys, and version 4 introduced
spelling and expansion extents. Graph artifact schema 5 adds public API roots; schema 4 added
general provider facts and suppressions; schema 3 added callable registration and escape facts;
schema 2 added target context.
Identity schema 1 remains independently versioned.

## Failure model

The index is all-or-nothing. A nonzero subprocess result, malformed AST JSON, resource limit, or
failed LibTooling action aborts analysis. Already collected facts are destroyed before reporting.
AST JSON subprocesses run in their own process groups; a wall-time limit, output limit, `SIGINT`, or
`SIGTERM` terminates Clang and its descendants with TERM followed by KILL after a short grace period.

## Run states

JSON report schema 10 carries these run states and the status of every selected translation unit:

- **complete**: every selected translation unit produced parseable AST facts and at least one
  context-appropriate root was found. Only a complete run may emit findings or return policy status
  2.
- **incomplete**: a supported analysis started, but a required translation unit, AST document, or
  root could not be indexed. The partial graph is discarded, no findings are emitted, and the
  process returns status 1.
- **unsupported**: the requested analysis context is not implemented, such as a mode other than
  `application`. The request is rejected before a report is produced and returns status 1.

Translation-unit status is `indexed`, `failed`, `skipped`, `timed_out`, `unsupported`, or
`cancelled`, with a stage and cause. Incomplete and unsupported JSON documents deliberately omit
roots and findings, and graph artifacts are written only after a complete index. Hard resource
limits are currently specific to AST JSON; LibTooling rejects them as unsupported because its
in-process action cannot be safely preempted.

An unsupported language or framework pattern inside an otherwise complete application run is an
analysis limitation, not a different run state. Such findings remain advisory and the limitation
must be recorded in the result ledger or a diagnostic until the frontend can model it
conservatively.

## Security boundary

Commands are passed as argument arrays, not shell strings. Existing output and dependency flags
are removed before the AST frontend adds its own syntax-only dependency manifest. As with normal
builds, the compilation database and project-local cache are trusted inputs: response files,
compiler plugins, and paths can still cause Clang to load repository-selected content. Cache data
is schema-checked and cannot bypass dependency-content validation; corrupt entries are rebuilt.
