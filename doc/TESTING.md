# Testing

Run everything with:

```sh
make test
```

That is equivalent to:

```sh
make selftest
make lsp-check
make standalone-check
make portability-check
```

## What each check covers

| Target | Command | Coverage |
| --- | --- | --- |
| `selftest` | `build/c-autotmpl-selftest` | Plugin core: decorator parsing, registry paths, block emission, presence macros, manual-block recognition, nested and deep chains, cross-template pruning. No clangd process. |
| `lsp-check` | `python3 tools/lsp-check.py <plugin>` | Live clangd with `--load`: generation from unsaved `didOpen` text, user-type guards, `#pragma once` rejection, later-open types, external edits to generated headers, pruning on `didChange`, nested dependency guards. |
| `standalone-check` | `python3 tools/standalone-check.py <cli>` | CLI: idempotency, direct and mangled usages, nested and deep chains, one-time `#pragma once` warning, `--dry-run`, guard text, presence macros. |
| `portability-check` | `make portability-check` | CLI links no LLVM, libclang, or C++ runtime (Linux uses `ldd`). |
| `bench` | `python3 tools/bench.py <cli> <plugin>` | Synthetic project: `clang -fsyntax-only`, CLI no-op/cold runs, clangd time to first diagnostics with/without module, plugin phase timings. |

`clangd --check` is **not** a substitute for `lsp-check`; it does not run AST
feature modules.

## Verbose output

```sh
C_AUTOTMPL_TEST_VERBOSE=1 make lsp-check
```

This prints the `[c-autotmpl]` log lines collected from clangd. Very useful when
a plugin regression fails.

For CLI debugging:

```sh
./build/autotmpl --root /tmp/test-project --dry-run --verbose src/
```

## The `test/` fixture project

`test/` contains a small C23 project with:

```text
tmpl/list.h
tmpl/box.h
gen/list.h
gen/box.h
trait/container.h
main.c
.clangd
```

It exercises `List(int)`, `Box(int)`, nested applications, and deep chains. A
typical manual check:

```sh
rm -f test/gen/list.h test/gen/box.h
./build/autotmpl --root test test/main.c
clang -std=c23 -Itest/trait -Itest test/main.c -o /tmp/autotmpl-test
/tmp/autotmpl-test
```

Expected output:

```text
BOX TEST:
46
LIST TEST:
36
24
12
99
```

## Benchmark arguments

```sh
make bench
make bench BENCH_ARGS="--files 80 --headers 2000 --reps 3"
```

The harness creates temporary sources, so it does not modify your repository.

## Adding a regression

- Pure generation / canonicalization behavior: add cases to
  `src/plugin/selftest.cpp` **and** `tools/standalone-check.py` so both
  implementations stay in sync.
- Editor behavior (unsaved buffers, pruning, mtime handling): add a step to
  `tools/lsp-check.py`.
- CLI-only portability or option behavior: add to
  `tools/standalone-check.py` or `make portability-check`.

## CI

GitHub Actions builds the CLI for macOS ARM, Linux x64, and Windows x64 on every
push and pull request, running `standalone-check.py` on each platform. See
[CI.md](CI.md). The clangd plugin is not built in CI because it must match the
target clangd revision and local build tree.
