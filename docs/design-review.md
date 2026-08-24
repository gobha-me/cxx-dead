# Design review

## Overall assessment

The proposal has the right central abstraction: dead-source analysis in C++ is a context-specific graph reachability problem, not a larger version of an unused-declaration warning. Its separation of indexing, project context, root discovery, graph analysis, confidence, and reporting is the strongest architectural choice and should be retained.

The proposed application-first sequence is also correct. It creates a falsifiable first milestone while preserving extension points for libraries and mixed workspaces. The emphasis on explanations and change-relative adoption is especially well matched to agent-generated repositories.

The draft should nevertheless tighten its correctness contract before calling the result "whole-program." A compilation database alone does not describe a whole linked program, and several of the proposed confidence scores imply more certainty than the available evidence supports.

## What is already strong

1. **Context is explicit.** Application and library roots cannot safely share one policy. Making context a first-class input prevents a fundamental category error.
2. **The graph is typed.** Calls, construction, address escapes, inheritance, overrides, exports, and registration have different reachability implications. Keeping edge kinds enables policy evolution.
3. **Precision is prioritized.** Conservative treatment of dynamic behavior is the right trust strategy.
4. **The engine is frontend-independent.** A neutral symbol graph makes the difficult C++ extraction layer replaceable and the algorithms easy to test.
5. **Differential adoption is planned.** `--fail-on-new` is likely more deployable than repository-wide enforcement.
6. **Explainability is a product requirement.** This is essential when findings may cause humans or agents to remove source.

## Gaps to resolve

### 1. Compilation does not imply linkage

`compile_commands.json` says how translation units are compiled; it does not say which objects are linked into an executable, which static-archive members are selected, or which custom link steps run. A database may contain several applications, tests, tools, mutually exclusive configurations, and generated probes.

The MVP should describe itself as analyzing "all listed translation units under an application assumption." Production target awareness requires link-command or build-system metadata. Until then, users should supply a target-restricted database.

### 2. Build configuration is part of symbol identity

Preprocessor definitions, language flags, target triples, and feature switches can produce different declarations from the same file. A workspace-wide index must retain a configuration identity and detect incompatible duplicate definitions rather than silently merging them.

Stable identity should ultimately combine Clang USR, linkage/mangled name, target/configuration, and translation unit for internal-linkage entities. Mangled names alone are insufficient for templates without emitted symbols, anonymous entities, and some declaration-only constructs.

### 3. SCCs are necessary but not sufficient for subsystem grouping

SCC analysis finds cycles that defeat zero-reference checks. It does not by itself identify an abandoned acyclic subsystem: a chain `A -> B -> C` produces three SCCs. Reporting should analyze the unreachable condensation graph and then use weak connectivity, ownership/type boundaries, or directory structure to construct useful architectural groups. SCC remains valuable evidence inside those groups.

### 4. Root and edge semantics need a formal policy

Each edge kind should declare whether it propagates reachability, lowers confidence, or adds a conditional root. For example, `ADDRESS_TAKEN` should not automatically behave like `CALLS`, while a recognized registration API may convert the callback into a live root or registration edge.

A provider API should return evidence records, not mutate an opaque root set. This enables explanations such as "rooted by installed header" or "retained because argument 1 escapes through configured registrar."

### 5. Implicit C++ behavior is a first-order concern

Constructors, destructors, base/member initialization, default arguments, conversion functions, operators, exception cleanup, allocation/deallocation functions, coroutines, and static initialization all create non-obvious call edges. These need explicit golden tests. A direct-call-only prototype is useful experimentally but should advertise incomplete language coverage.

Static initialization also depends on linkage. A namespace-scope initializer in an object file is live only when that object is part of the final link; archive extraction complicates this further.

### 6. Virtual reachability needs receiver feasibility

"All overrides" is a safe first approximation, but only within linked types that can actually instantiate or escape into the receiver set. Class hierarchy analysis should eventually track constructed dynamic types. Pure declarations and externally supplied subclasses require different handling in applications and libraries.

### 7. Type-level deadness needs type evidence

An unreachable set of methods is not identical to an unused type. Types may be used for layout, traits, overload selection, templates, serialization metadata, or ABI without a runtime method call. `OWNS_MEMBER`, `USES_TYPE`, `INSTANTIATES`, and layout/RTTI evidence are needed before a type-level classification becomes stronger than an aggregate presentation of method findings.

### 8. Templates need policy by exposure, not merely instantiation

Application-private templates can be judged against observed instantiations. Public header templates are source APIs whose future external instantiations are unknowable. The index should distinguish template patterns, specializations, and instantiations and let context decide which are candidates.

### 9. Numeric confidence should not initially look statistical

Values such as 97% are understandable, but they are not calibrated probabilities. Early versions should make evidence categories authoritative and describe scores as policy weights. Calibration can later use a reviewed corpus and track precision per classification.

### 10. Differential analysis requires stable semantics

Comparing two Git revisions is more than subtracting symbol sets. The tool must reproduce both build configurations, map renamed/moved symbols, distinguish newly unreachable symbols from newly introduced symbols, and report incomplete baselines. A stable JSON schema and stable symbol identity must precede `--fail-on-new`.

### 11. Generated and macro-expanded code needs dual locations

The graph should retain spelling and expansion locations. Generated code often should be included in graph construction but excluded or remapped in reporting. A single source path loses that distinction.

### 12. Indexing must be incremental and bounded

Full AST JSON is suitable for a dependency-light experiment, but it includes enormous system-header subtrees and is not a scalable final index. The production frontend should use LibTooling/clangd index callbacks, stream facts to storage, hash compilation inputs, and avoid retaining full AST documents.

## Recommended decisions

- Keep `application` as the only initial context and require a target-restricted compilation database.
- Treat analysis as incomplete—and do not emit a success policy result—if any translation unit fails to index.
- Use symbolic confidence classes as the policy surface; retain scores only as provisional metadata.
- Make roots carry evidence and provenance.
- Traverse only semantically live edge kinds. Treat address escape as uncertainty until a provider explains it.
- Add implicit-call golden tests before expanding the supported-language claim.
- Keep SCC as a core algorithm, but add unreachable weak-component aggregation later.
- Establish stable schema and symbol IDs before Git differential analysis.
- Replace full AST dumps before performance work on large repositories.

## Prototype decision

The implementation in this repository uses Clang's command-line JSON AST because Clang development headers are not assumed. This validates extraction and graph semantics with no third-party dependency. The `ClangAstIndexer` boundary is intentionally replaceable; it is not proposed as the long-term large-project indexer.
