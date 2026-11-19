# Building c-autotmpl

c-autotmpl ships two independent deliverables:

| Deliverable | Source | Build cost | What it is for |
| --- | --- | --- | --- |
| `autotmpl` CLI | `src/cli/autotmpl.c` | One C99 compile, ~1 second | Scripts, build systems, CI, pre-save generation |
| `libc-autotmpl.so` clangd module | `src/plugin/*.cpp` | Requires a matching LLVM/clangd source tree | Interactive generation from unsaved buffers, AST-aware guards, pruning |

The CLI is deliberately boring: it is one C file and any C99 compiler can build
it. The clangd module is the complicated path, because clangd feature modules
use clangd's **internal** C++ API. This document spends most of its space on
that path.

---

## 1. Build the CLI (the short path)

The whole program is one file:

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
```

No include paths, generated headers, libraries, LLVM, libclang, compilation
database, or C++ runtime are needed. ISO C99 has no directory API, so the same
file contains a small conditionally compiled POSIX/Win32 filesystem shim near
the top; everything above the scanner is plain C99.

Equivalent builds:

```sh
# clang, macOS or Linux
clang -std=c99 -O2 src/cli/autotmpl.c -o autotmpl

# MSVC, Windows (from a Developer Command Prompt)
cl /nologo /TC /std:c11 /O2 /W3 /Fe:autotmpl.exe src\cli\autotmpl.c

# via the Makefile
make build/autotmpl
make portability-check
```

`make install` installs this binary as `~/.local/bin/autotmpl`. On Linux,
`make portability-check` verifies that it links only libc (and the loader).

Typical use:

```sh
./autotmpl --root . --verbose src/ include/
./autotmpl --root . --dry-run --verbose src/main.c
```

See [USAGE.md](USAGE.md) for all options and [CI.md](CI.md) for the prebuilt
binaries produced by GitHub Actions.

---

## 2. Build the clangd feature module (the long path)

### 2.1 What “build against your clangd” actually means

The module is loaded with `clangd --load=/absolute/path/to/libc-autotmpl.so`.
It is a shared library compiled from clangd internal headers. It is **not** a
stable plugin ABI in the way libclang is.

Three consequences matter:

1. **Use headers from the same LLVM/clangd revision as the clangd binary.**
   The module includes private clangd headers such as `LSPBinder.h` and even
   temporarily redefines `private` to wrap notification handlers. A different
   revision can change layouts, vtable order, or signatures.

2. **Match the compiler ABI and build flags.** clangd is normally built with
   exceptions and RTTI disabled and with a specific libstdc++ ABI. The Makefile
   defaults to `-fno-exceptions -fno-rtti` and
   `-D_GLIBCXX_USE_CXX11_ABI=1`. If your clangd uses ABI 0, pass `LLVM_ABI=0`.

3. **Do not link LLVM or clangd into the module.** The Makefile deliberately
   links only `-shared`; undefined symbols are resolved from the clangd process
   that loads it. Linking a second copy of LLVM/clangd usually produces
   duplicate-symbol or version-skew failures at load time.

The upside is that the module itself has no LLVM/clangd *link* dependency and
adds no separate server or editor plugin. The only editor setup is the
`--load` flag.

### 2.2 Prerequisites

You need:

- the LLVM project source tree that produced your clangd, including
  `clang/`, `llvm/`, and `clang-tools-extra/`;
- a configured LLVM build tree with generated headers (the tree that built
  clangd);
- `llvm-config` from that build tree;
- a C++17 compiler compatible with the one used for clangd (`g++` or
  `clang++`);
- `ninja` (or another generator supported by CMake), if you build LLVM yourself;
- Python 3 only if you want to run the integration checks.

Check the clangd you intend to load the module into:

```sh
clangd --version
which clangd
```

If you use a packaged clangd (distribution package, Homebrew, LLVM release
tarball), try to obtain the matching source tag first. If that is not possible,
build clangd yourself from a known revision and use that same clangd in your
editor.

### 2.3 Produce a matching LLVM/clangd tree

If you do not already have one:

```sh
git clone --depth 1 https://github.com/llvm/llvm-project.git
cd llvm-project

cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
  -DLLVM_TARGETS_TO_BUILD=Native

cmake --build build --target clangd llvm-config
```

This gives you:

```text
llvm-project/                       LLVM_SRC
llvm-project/clang-tools-extra/     CLANGD_SRC (derived by default)
llvm-project/build/                 LLVM_BUILD
llvm-project/build/bin/llvm-config  LLVM_CONFIG (derived by default)
llvm-project/build/bin/clangd       the clangd to run with --load
```

The generated headers the Makefile expects include:

```text
$(LLVM_BUILD)/tools/clang/tools/extra/clangd
$(LLVM_BUILD)/tools/clang/include
$(LLVM_BUILD)/include
```

If your build directory places generated headers somewhere else, adjust the
`INCLUDES` variable in the `Makefile`.

### 2.4 Point the Makefile at that tree

The defaults in the Makefile are the author's local build tree. Override them
on the command line (or with environment variables):

```sh
make \
  LLVM_SRC=/path/to/llvm-project \
  LLVM_BUILD=/path/to/llvm-project/build \
  CLANGD_SRC=/path/to/llvm-project/clang-tools-extra
```

`CLANGD_SRC` defaults to `$(LLVM_SRC)/clang-tools-extra`, so in the layout above
you can omit it:

```sh
make LLVM_SRC=/path/to/llvm-project LLVM_BUILD=/path/to/llvm-project/build
```

Useful variables:

| Variable | Default | Purpose |
| --- | --- | --- |
| `LLVM_SRC` | author's local checkout | Source tree with `llvm/`, `clang/`, `clang-tools-extra/` |
| `CLANGD_SRC` | `$(LLVM_SRC)/clang-tools-extra` | clangd source headers |
| `LLVM_BUILD` | author's local build | Configured build tree with generated headers |
| `LLVM_CONFIG` | `$(LLVM_BUILD)/bin/llvm-config` | Supplies include/library dirs for the selftest |
| `LLVM_ABI` | `1` | Value of `_GLIBCXX_USE_CXX11_ABI`; set to `0` if clangd uses the old ABI |
| `CXX` | `g++` | C++ compiler; use `clang++` if that built clangd |
| `CC` | `cc` | C compiler for the CLI |

If in doubt, inspect how clangd was configured:

```sh
grep -R '_GLIBCXX_USE_CXX11_ABI' /path/to/llvm-project/build/CMakeCache.txt
```

### 2.5 Compile the module

```sh
make build/libc-autotmpl.so
# or, with all overrides:
make -j"$(nproc)" \
  LLVM_SRC=/path/to/llvm-project \
  LLVM_BUILD=/path/to/llvm-project/build \
  LLVM_ABI=1 \
  build/libc-autotmpl.so
```

The Makefile compiles:

```text
src/plugin/AutotmplModule.cpp   feature module, LSP wrapping, AST listener
src/plugin/SourceScan.cpp       usage discovery and nested canonicalization
src/plugin/TemplateRegistry.cpp decorator parsing, root-relative paths, guards
src/plugin/TypeIndex.cpp        text index of guarded type declarations
src/plugin/Instantiator.cpp     block emission, marker parsing, pruning
```

with roughly:

```text
-std=c++17 -O2 -fPIC -fno-exceptions -fno-rtti
-fvisibility-inlines-hidden -fno-semantic-interposition
-I<clangd source/include dirs> -I<generated build dirs> -Isrc/plugin
```

and links with:

```sh
$(CXX) -shared -o build/libc-autotmpl.so build/plugin/*.o
```

There is intentionally no `-lLLVM`, `-lclang`, or `-lclangd`. If you add one
while experimenting, remove it again.

`make -j4` builds both the plugin and the CLI. `make install` copies the plugin
to `~/.local/lib/c-autotmpl/libc-autotmpl.so` and the CLI to
`~/.local/bin/autotmpl`.

### 2.6 Load it into clangd

Use an **absolute** path:

```sh
clangd \
  --load=/absolute/path/to/build/libc-autotmpl.so \
  --background-index
```

For Neovim with `nvim-lspconfig`, that is the entire setup:

```lua
require("lspconfig").clangd.setup({
  cmd = {
    "clangd",
    "--load=/home/you/Projects/c-autotmpl/build/libc-autotmpl.so",
    "--background-index",
  },
})
```

No separate editor plugin, sidecar process, or compile database is required for
generation. A compile database may still be useful to clangd itself for include
paths and defines.

Run clangd with `--log=verbose` the first time. You should see lines like:

```text
[c-autotmpl] scanning /path/to/project, found 2 template(s)
[c-autotmpl] generated List(int) in /path/to/project/gen/list.h
[c-autotmpl] workspace scan complete
```

Generation and pruning happen on `didOpen` and `didChange`, before clangd builds
the preamble. The AST listener is only a fallback for cases such as macro
expansion; it can also offer an include-fix for the generated header.

Restart clangd/Neovim after building or replacing the module. dlopen happens at
process start.

### 2.7 Platform notes

#### Linux

This is the primary development platform. The filename `libc-autotmpl.so` is
what the Makefile produces and what the examples use.

#### macOS

Build with the clang++ that matches your clangd, using `LLVM_SRC`, `LLVM_BUILD`,
and `LLVM_ABI` as above. The `.so` name works with full-path `dlopen` on macOS,
but if your clangd packaging expects `.dylib`, copy or rename the result:

```sh
cp build/libc-autotmpl.so build/libc-autotmpl.dylib
clangd --load=/absolute/path/to/build/libc-autotmpl.dylib
```

Make sure the binary is the same architecture as clangd. GitHub Actions builds
the **CLI** for macOS ARM (`macos-15`), not the plugin.

#### Windows

Feature modules require clangd to be built with dynamic loading support, and the
module must be a DLL built with the same MSVC ABI as clangd. The Makefile in
this repository targets POSIX shared libraries and has not been exercised for a
Windows plugin build. Expect to adapt the compiler flags, suffix, and include
paths. The `autotmpl` CLI, by contrast, is built for Windows x64 in CI with MSVC
`/std:c11` and works without LLVM.

### 2.8 Verify the module

There are two levels of checks.

#### Core unit check

```sh
make selftest
```

This links the plugin core against LLVM support libraries and tests template
discovery, block emission, nested expansion, deep chains, and cross-template
pruning. It does not load clangd.

#### Live LSP check

```sh
make lsp-check
# or directly:
python3 tools/lsp-check.py /absolute/path/to/build/libc-autotmpl.so
```

This starts a real clangd with `--load`, opens a file whose **unsaved buffer**
contains usages, and verifies generation, guards, pruning, external-edit
handling, and nested dependency guards. `clangd --check` is not enough: it does
not run AST feature modules.

To see plugin log lines:

```sh
C_AUTOTMPL_TEST_VERBOSE=1 make lsp-check
```

`make test` runs `selftest`, `lsp-check`, `standalone-check`, and
`portability-check`.

### 2.9 Runtime environment variables

| Variable | Effect |
| --- | --- |
| `C_AUTOTMPL_ROOT` | Forces the workspace root instead of auto-discovery. Useful for unusual layouts. |
| `C_AUTOTMPL_TIMING=1` | Logs phase timings: scan, usages, prune, generate, workspace scan. |
| `C_AUTOTMPL_TEST_VERBOSE=1` | Makes `lsp-check` print plugin log lines (test-only). |

Root discovery walks upward from the open file (up to eight parents). It first
looks for a `tmpl/` or `templates/` directory containing a template decorator,
then falls back to `.clangd`, `compile_commands.json`, or `.git`. This two-pass
behavior prevents a nested `.clangd` with only compile flags from hiding an
ancestor's templates.

A background thread performs the first full workspace usage scan and refreshes
it periodically. Pruning starts once that scan completes; generation from the
current buffer works immediately.

### 2.10 Upgrade checklist

When you upgrade clangd:

1. Check out the matching LLVM revision.
2. Rebuild clangd and `llvm-config`.
3. Rebuild the module with the same `LLVM_SRC`, `LLVM_BUILD`, `CXX`, and
   `LLVM_ABI`.
4. Run `make selftest` and `make lsp-check`.
5. Restart the editor so clangd dlopens the new module.
6. Remove stale copies from `~/.local/lib/c-autotmpl/` if you load by a
   different path.

### 2.11 Build/load troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `fatal error: 'FeatureModule.h' file not found` | `CLANGD_SRC` or generated include dirs are wrong. | Point `LLVM_SRC`/`LLVM_BUILD` at the tree that built clangd; adjust `INCLUDES` if needed. |
| Undefined symbol when clangd starts | Module ABI does not match the loaded clangd. | Rebuild against that exact clangd; match `CXX`, exception/RTTI flags, and `LLVM_ABI`. |
| `dlopen` says “wrong architecture” | macOS binary and clangd arch differ. | Build with the same arch; use the arm64 or Intel toolchain as appropriate. |
| clangd starts but `[c-autotmpl]` never appears | Wrong `--load` path, or clangd was started before the module existed. | Use an absolute path and restart clangd. |
| `scanning ..., found 0 template(s)` | Root discovery missed the header, or no decorator was parsed. | Check the decorator syntax, or set `C_AUTOTMPL_ROOT`. |
| No generation for a user type | Type header lacks a traditional include guard. | See [TEMPLATES.md](TEMPLATES.md) and [TROUBLESHOOTING.md](TROUBLESHOOTING.md). |
| Pruning never happens | Full workspace scan has not completed yet. | Wait for `workspace scan complete` in the verbose log. |

---

## 3. Testing both paths

```sh
make                         # plugin + CLI
make selftest                # plugin core
make lsp-check               # live clangd
make standalone-check        # CLI
make portability-check       # CLI has no LLVM/libclang/C++ runtime link
make test                    # all of the above
make bench                   # optional overhead harness
```

See [TESTING.md](TESTING.md) for what each check covers.
