# Continuous integration and release binaries

The workflow lives at `.github/workflows/cli-build.yml` and is named
**CLI binaries**.

## Triggers

```yaml
on:
  push:
    branches: ["**"]
    tags: ["v*"]
  pull_request:
  workflow_dispatch:
```

Every push and pull request builds and smoke-tests the CLI. Pushing a `v*`
tag additionally publishes the artifacts to a GitHub release.

## Matrix

| Target | GitHub runner | Compiler | Artifact |
| --- | --- | --- | --- |
| macOS ARM64 | `macos-15` (Apple Silicon) | Apple `clang` | `autotmpl-macos-arm64` |
| Linux x86-64 | `ubuntu-latest` | `gcc` | `autotmpl-linux-x86_64` |
| Windows x86-64 | `windows-latest` | MSVC via `ilammy/msvc-dev-cmd@v1`, `/std:c11` | `autotmpl-windows-x86_64.exe` |

Each job:

1. checks out the repository;
2. compiles `src/cli/autotmpl.c` directly with warnings enabled;
3. runs `--help`;
4. runs `python tools/standalone-check.py <binary>`;
5. uploads the executable as a GitHub Actions artifact.

POSIX jobs compile with:

```sh
cc -std=c99 -O2 -Wall -Wextra -pedantic-errors src/cli/autotmpl.c -o <artifact>
```

Windows compiles with:

```powershell
cl /nologo /TC /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:<artifact> src\cli\autotmpl.c
```

The source itself defines `_CRT_SECURE_NO_WARNINGS` for MSVC and includes the
Windows headers needed for `_stat`, `_access`, and directory enumeration.

## Release behavior

The `release` job runs only for `refs/tags/v*`. It:

1. downloads all `autotmpl-*` artifacts into `dist/`;
2. writes `SHA256SUMS`;
3. uses the GitHub CLI (`gh release create` or `gh release upload --clobber`)
   to attach all files to the tag's release.

It needs `contents: write` permission; the build jobs only need `contents: read`.

## Downloading artifacts

From a workflow run page, open the **Artifacts** section and download the
matching target. Rename it to `autotmpl` (or `autotmpl.exe` on Windows) and put
it on your `PATH`:

```sh
chmod +x autotmpl-linux-x86_64
mv autotmpl-linux-x86_64 autotmpl
./autotmpl --help
```

## Why the clangd plugin is not built here

A portable plugin binary does not exist. clangd feature modules use internal
headers and must match the exact clangd revision, compiler ABI, exception/RTTI
settings, and libstdc++ ABI of the process that loads them. Building a generic
`.so` in CI would produce a binary that only works with the CI image's clangd,
if at all.

Build the plugin locally with [BUILD.md](BUILD.md). The CLI is the artifact we
can meaningfully ship for macOS ARM, Linux, and Windows.

## Reproducing a matrix job locally

```sh
# Linux / macOS
clang -std=c99 -O2 -Wall -Wextra -pedantic-errors \
  src/cli/autotmpl.c -o /tmp/autotmpl-local
python3 tools/standalone-check.py /tmp/autotmpl-local

# Windows, in a Developer Command Prompt
cl /nologo /TC /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:autotmpl.exe src\cli\autotmpl.c
python tools\standalone-check.py autotmpl.exe
```

## Adding another CLI target

1. Add an entry to the `matrix.include` list with `name`, `os`, `artifact`,
   `cc`, and `python` values.
2. If it is not Windows, the existing POSIX build step handles it as long as
   the runner has a C99 compiler and Python.
3. If it needs special flags, add a conditional step similar to the MSVC one.
4. Update the table in this document.
