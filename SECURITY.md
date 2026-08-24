# Security policy

## Supported versions

`cxx-dead` is currently pre-alpha and has no supported release line. Security fixes are applied to
the default branch.

## Reporting a vulnerability

Do not open a public issue for a vulnerability. Use GitHub's **Security → Report a vulnerability**
flow for this repository. Include the affected revision, impact, reproduction steps, and any
suggested mitigation.

The analyzer executes Clang commands derived from a compilation database. Compilation databases,
response files, compiler plugins, source trees, and build artifacts must be treated as trusted
input. Do not run `cxx-dead` against an untrusted repository outside an appropriate sandbox.
