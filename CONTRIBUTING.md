# Contributing to cxx-dead

`cxx-dead` is pre-alpha. Correctness and explainability take priority over finding the largest
possible number of candidates. A smaller set of trustworthy findings is the goal.

## Reporting analyzer problems

Use the dedicated false-positive form when live code is reported as unreachable. Useful reports
include:

- the `cxx-dead` revision and Clang version;
- the exact command used;
- the relevant compilation database entry, with private paths or flags removed;
- the unexpected finding in JSON form;
- why the symbol is live, including any callback, registration, plugin, or external-consumer path;
- ideally, a reduced C++ fixture that preserves the behavior.

Do not post credentials, proprietary source, or an unredacted compilation database in a public
issue.

## Building and testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Please add or update a golden fixture for analysis behavior changes. A bug fix should demonstrate
the incorrect behavior before the fix and the intended result afterward.

## Pull requests

Keep changes focused and explain their effect on false positives, false negatives, runtime, and
memory where applicable. Update the analysis contract or architecture documentation when semantics
change. By contributing, you agree that your contribution is licensed under the repository's BSD
3-Clause License.
