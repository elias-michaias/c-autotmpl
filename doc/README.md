# c-autotmpl documentation

Start here, then follow the path you actually use.

## If you just want the executable

- [USAGE.md](USAGE.md) — CLI options, build-script integration, editor setup.
- [TEMPLATES.md](TEMPLATES.md) — how to write template headers and type usages.
- [CI.md](CI.md) — prebuilt macOS ARM / Linux / Windows CLI artifacts.

## If you want to build the clangd plugin

- [BUILD.md](BUILD.md) — **the main build guide**. It covers the trivial CLI build
  and then goes deep into the clangd feature module: matching source trees,
  generated headers, ABI flags, loading, verification, and troubleshooting.
- [ARCHITECTURE.md](ARCHITECTURE.md) — what the plugin does on `didOpen` /
  `didChange`, how pruning works, and how the CLI mirrors the same policy.
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — when generation, guards, or plugin
  loading do not behave as expected.

## If you are changing the project

- [ARCHITECTURE.md](ARCHITECTURE.md) — component map and data flow.
- [TESTING.md](TESTING.md) — what each check exercises and how to run them.
- [CI.md](CI.md) — release workflow and how to add another CLI target.
- [BUILD.md](BUILD.md) — plugin build internals and upgrade checklist.

## Repository map

```text
src/cli/      one self-contained C99 CLI source file
src/plugin/   clangd feature module and its C++ core
tools/        Python integration checks and benchmark harness
test/         small C23 fixture project
doc/          this documentation
.github/      CLI build/release workflow
```
