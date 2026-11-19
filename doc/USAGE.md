# Using c-autotmpl

## Which path should I use?

| Situation | Use |
| --- | --- |
| You want deterministic generation in a build script or CI job | `autotmpl` CLI |
| You want a tiny executable to ship with a project | `autotmpl` CLI |
| You want headers generated while you type, from unsaved buffers | clangd module |
| You want unused instantiations removed automatically | clangd module |
| You want the AST to help find a type's declaring header | clangd module |

Both paths write the same generated headers. Many projects use both: the plugin
for editor feedback and the CLI as the reproducible build step.

See [TEMPLATES.md](TEMPLATES.md) before using either path; guard and include
order rules are shared.

---

## Standalone CLI

Build it in one command:

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
```

Or use the prebuilt binary from [CI.md](CI.md).

### Options

```text
usage: autotmpl [--root DIR] [--dry-run] [--verbose] <file-or-dir>...

  --root DIR, -r DIR   workspace root for template/type discovery
  --dry-run, -n        report changes without writing files
  --verbose, -v        print generated blocks and summary counters
  --                   treat remaining arguments as files/directories
  --help, -h           show usage and exit
```

Inputs may be files or directories. Directories are walked recursively in
sorted order, with files visited before subdirectories. Source extensions are
`.c`, `.h`, `.cc`, `.cpp`, `.cxx`, `.hh`, `.hpp`, and `.hxx`. Hidden
directories, `build/`, `node_modules/`, and `gen/` are skipped.

Without `--root`, the CLI uses the common ancestor of all input files. For
predictable results in scripts, pass `--root .` explicitly.

### Examples

```sh
# Scan an entire project
autotmpl --root . src include

# See what would change
autotmpl --root . --dry-run --verbose src/main.c

# Generate from one source file
autotmpl --root . src/main.c
```

A verbose run prints lines like:

```text
generated List(int) in /project/gen/list.h
generated Box(int) in /project/gen/box.h
templates=2 added=2 skipped=0
```

A second run should report `added=0`. The CLI is additive and idempotent; it
does **not** prune unused instantiations. Use the plugin for pruning.

### Build integration

```make
.PHONY: gen all
gen:
	autotmpl --root . src include

all: gen
	$(CC) $(CFLAGS) src/*.c -o app
```

The CLI has no compile-database requirement, so it can run before any generated
files exist. It is fast enough (~1–2 ms in the benchmark project) to run on
every build.

---

## clangd module

Build it against your clangd as described in [BUILD.md](BUILD.md), then add one
flag to clangd:

```sh
clangd --load=/absolute/path/to/libc-autotmpl.so
```

For Neovim with `nvim-lspconfig`:

```lua
require("lspconfig").clangd.setup({
  cmd = {
    "clangd",
    "--load=/home/you/Projects/c-autotmpl/build/libc-autotmpl.so",
    "--background-index",
  },
})
```

That is the whole editor-side setup. Restart clangd after changing the module.

On `didOpen` and `didChange`, the plugin:

- scans the unsaved buffer for usages;
- writes missing blocks before clangd parses the file;
- finds user-defined type guards via a text index and the clang AST;
- prunes unused blocks once the background workspace scan completes;
- reloads generated headers you edit externally.

A compile database is not required for generation, but clangd still needs
correct include paths and defines to parse your project.

### Environment variables

| Variable | Effect |
| --- | --- |
| `C_AUTOTMPL_ROOT` | Force the workspace root instead of auto-discovery. |
| `C_AUTOTMPL_TIMING=1` | Log phase timings to clangd's stderr. |
| `C_AUTOTMPL_TEST_VERBOSE=1` | Print plugin log lines from `lsp-check`. |

Run clangd with `--log=verbose` to see normal plugin activity:

```text
[c-autotmpl] scanning /project, found 2 template(s)
[c-autotmpl] generated List(int) in /project/gen/list.h
[c-autotmpl] workspace scan complete
[c-autotmpl] removed /* c-autotmpl: List(Foo) */ from /project/gen/list.h
```

---

## Generated files

Generated headers live in a sibling `gen/` directory. They are normal C headers
with include guards and marker comments. You can commit them for reproducible
builds, or add them to `.gitignore` and regenerate.

Do not remove the `/* c-autotmpl: ... */` markers if you want pruning to work.
Hand-written blocks are tolerated, but marked blocks are the canonical format.

For nested types, include generated headers in dependency order:

```c
#include "gen/box.h"
#include "gen/list.h"
```

See [TEMPLATES.md](TEMPLATES.md) for the complete nested-type policy.

---

## Performance expectations

- CLI no-op/cold generation: about 1–2 ms even with thousands of unrelated
  headers.
- Plugin first-open work: a few tenths of a millisecond in the benchmark
  project; clangd parse overhead is within noise.
- Workspace scanning runs on a background thread so keystrokes do not wait.

Run `make bench` to reproduce the measurements on your machine.
