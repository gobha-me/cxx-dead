# Target-aware build model

`compile_commands.json` describes compilation but not which objects form an executable or library.
Target mode combines the compilation database with either a CMake File API codemodel-v2 reply or an
explicit JSON manifest, then indexes one selected target's transitive link closure.

## CMake File API input

Create the stateless codemodel query before configuring the build:

```bash
cmake -E make_directory build/.cmake/api/v1/query
cmake -E touch build/.cmake/api/v1/query/codemodel-v2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cxx-dead --cmake-build-dir build --configuration Debug --target app
```

The adapter reads the newest `index-*.json` reply, requires codemodel major version 2, and tolerates
new minor versions by consuming only documented fields. Referenced reply files must stay within the
reply directory. C++ source membership, target ids, artifacts, and target dependencies are retained;
dependencies whose CMake backtrace is only `add_dependencies()` are not treated as link membership.

The compilation database defaults to `<build>/compile_commands.json`. When one source is compiled by
multiple targets, the optional compilation-database `output` field is used to select the owning CMake
target. If distinct commands remain ambiguous, analysis fails instead of guessing.

## Explicit manifest fallback

`--target-manifest path.json` accepts schema version 1:

```json
{
  "schema_version": 1,
  "source_root": "/workspace/project",
  "build_root": "/workspace/project/build",
  "configurations": [
    {
      "name": "Debug",
      "targets": [
        {
          "id": "core",
          "name": "core",
          "type": "static_library",
          "sources": ["src/core.cpp"],
          "dependencies": [],
          "artifacts": ["libcore.a"]
        },
        {
          "id": "app",
          "name": "app",
          "type": "executable",
          "sources": ["src/main.cpp"],
          "dependencies": ["core"],
          "artifacts": ["app"]
        }
      ]
    }
  ]
}
```

`source_root` and `build_root` are resolved relative to the manifest. Source paths are relative to
`source_root`; artifact paths are relative to `build_root`. Supported target types are
`executable`, `object_library`, `static_library`, `shared_library`, `module_library`,
`interface_library`, and `utility`. Dependencies refer to target ids. Interface and utility targets
may carry dependency relationships but do not contribute compiled source files to an analysis
closure; utility targets cannot be selected.

## Selection and semantics

- `--configuration` is required when metadata contains multiple configurations. Its name becomes
  the stable configuration identity unless `--configuration-id` overrides it.
- `--target` accepts a target name or id. It is required when more than one executable exists.
  Production and test policies are separate invocations against their respective targets; names are
  never heuristically classified.
- Executable sources and transitive object-library objects participate directly. Calls traverse
  into linked shared/module libraries, but uncalled exported APIs are not automatically rooted yet.
- Every static-library translation unit in the closure is indexed. Exact archive-member extraction,
  whole-archive flags, and conditional static initializers are not modeled; the report emits a
  diagnostic for this conservative approximation.
- Missing dependency metadata is diagnosed. Missing or ambiguous required compilation commands,
  malformed replies/manifests, and a closure with no application root fail the run without findings.

Report schema 6 and graph artifact schema 2 expose the configuration, target id/name/kind, and the
sorted closure target names. Symbol identity schema 1 is unchanged.
