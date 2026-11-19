# Building c-autotmpl

There are two separate builds.

The CLI is one C99 source file. It needs no LLVM, libclang, clang headers, C++
runtime, or compile database.

The clangd module is a C++ shared library built from clangd internal headers. It
must match the clangd binary that loads it, so the build takes more setup.

## CLI

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
```

Any C99 compiler works. ISO C99 has no directory API, so the same file contains
a small POSIX/Win32 filesystem shim near the top.

On Windows with MSVC:

```bat
cl /nologo /TC /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:autotmpl.exe src\cli\autotmpl.c
```

The source defines `_CRT_SECURE_NO_WARNINGS` for MSVC and includes the Windows
headers needed by the shim.

With the Makefile:

```sh
make build/autotmpl
make portability-check
make install          # installs to ~/.local/bin/autotmpl
```

`portability-check` fails if the CLI links LLVM, libclang, or a C++ runtime.

GitHub Actions builds and tests the CLI for macOS ARM64, Linux x86-64, and
Windows x86-64. Tag pushes named `v*` attach the binaries and a `SHA256SUMS`
file to a GitHub release. The workflow is `.github/workflows/cli-build.yml`.

## clangd plugin

> **Upstream note.** clangd's feature module interface is now in LLVM. When
> distributions expose that interface in their normal clangd development
> packages, this module should be buildable against the system clangd instead of
> a private source tree. It still uses clangd internals today, so the headers
> must match your clangd binary. That packaging gap is the main thing likely to
> improve.

Check which clangd you actually run:

```sh
clangd --version
which clangd
```

Before cloning anything, see whether your package manager already shipped the
needed headers:

```sh
find /usr/include /usr/local/include /usr/lib /usr/local/lib \
  -path '*/clangd/FeatureModule.h' 2>/dev/null | head
find /usr/bin /usr/local/bin /usr/lib /usr/local/lib \
  -name 'llvm-config*' -type f 2>/dev/null | head
```

If both are present and match your clangd version, point `LLVM_SRC`,
`LLVM_BUILD`, `CLANGD_SRC`, and `LLVM_CONFIG` at that installation. The Makefile
expects a source-tree layout, so you may need to adjust `INCLUDES`. If the
headers are missing, use the shallow clone below.

### What you need today

- The LLVM source tree used to build your clangd. Only `llvm`, `clang`, and
  `clang-tools-extra` matter here.
- The configured LLVM build tree that generated clangd's headers.
- `llvm-config` from that build tree.
- Git, CMake, Ninja, and a C++17 compiler compatible with clangd.
- Python 3 if you want to run the integration checks.

### Get a matching LLVM tree

A partial, shallow, sparse checkout is enough. This skips history and projects
such as mlir, libc, and flang. If you want to match a packaged clangd, use its
LLVM release tag from `clangd --version`, for example `llvmorg-20.1.8`:

```sh
git clone --depth 1 --single-branch --filter=blob:none --sparse \
  --branch llvmorg-20.1.8 \
  https://github.com/llvm/llvm-project.git llvm-project
cd llvm-project
git sparse-checkout set llvm clang clang-tools-extra
```

If you are building clangd yourself from current LLVM, omit the `--branch` line.
If your Git or remote does not support blob filtering, drop
`--filter=blob:none`. The depth, single-branch, and sparse settings still help.

Then configure and build only what is needed for matching headers:

```sh
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
  -DLLVM_TARGETS_TO_BUILD=Native \
  -DLLVM_ENABLE_EH=OFF \
  -DLLVM_ENABLE_RTTI=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_TESTS=OFF

cmake --build build --target clangd llvm-config
```

You still build clangd today because the generated headers must match the
binary you load the module into. Once system packages ship those headers, this
step should go away.

This gives the layout the Makefile expects:

```text
llvm-project/                       LLVM_SRC
llvm-project/clang-tools-extra/     CLANGD_SRC
llvm-project/build/                 LLVM_BUILD
llvm-project/build/bin/llvm-config  LLVM_CONFIG
llvm-project/build/bin/clangd       the clangd to run with --load
```

Generated headers come from `$(LLVM_BUILD)/tools/clang/tools/extra/clangd`,
`$(LLVM_BUILD)/tools/clang/include`, and `$(LLVM_BUILD)/include`. If your build
tree puts them elsewhere, edit `INCLUDES` in the Makefile.

### Configure the build

The defaults in the Makefile point at the author's local tree. Override them:

```sh
make \
  LLVM_SRC=/path/to/llvm-project \
  LLVM_BUILD=/path/to/llvm-project/build
```

`CLANGD_SRC` defaults to `$(LLVM_SRC)/clang-tools-extra`, so you only need to
set it for a nonstandard layout.

| Variable | Default | Meaning |
| --- | --- | --- |
| `LLVM_SRC` | local checkout | Source tree root |
| `LLVM_BUILD` | local build | Configured build tree |
| `CLANGD_SRC` | `$(LLVM_SRC)/clang-tools-extra` | clangd source headers |
| `LLVM_CONFIG` | `$(LLVM_BUILD)/bin/llvm-config` | Used by the selftest |
| `LLVM_ABI` | `1` | `_GLIBCXX_USE_CXX11_ABI` value for clangd |
| `CXX` | `g++` | Use `clang++` if that built clangd |

If clangd was built with the old libstdc++ ABI, pass `LLVM_ABI=0`. The plugin is
built with `-fno-exceptions` and `-fno-rtti` to match normal LLVM/clangd builds.

### Build and link

```sh
make -j4 \
  LLVM_SRC=/path/to/llvm-project \
  LLVM_BUILD=/path/to/llvm-project/build \
  build/libc-autotmpl.so
```

The plugin sources are:

```text
src/plugin/AutotmplModule.cpp   registration, LSP wrapping, AST listener
src/plugin/SourceScan.cpp       usage finding, nested canonicalization
src/plugin/TemplateRegistry.cpp decorators, paths, include guards
src/plugin/TypeIndex.cpp        guarded type lookup
src/plugin/Instantiator.cpp     block writing, marker parsing, pruning
```

They compile as C++17 with `-fPIC` and the clangd include paths above. The link
step is only:

```sh
$(CXX) -shared -o build/libc-autotmpl.so build/plugin/*.o
```

There is no `-lLLVM`, `-lclang`, or `-lclangd`. Undefined symbols are resolved
from the clangd process. Adding those libraries usually causes duplicate or
mismatched symbols at load time.

`make install` copies the module to
`~/.local/lib/c-autotmpl/libc-autotmpl.so`.

### Load it

Use an absolute path:

```sh
clangd --load=/absolute/path/to/build/libc-autotmpl.so
```

With Neovim and `nvim-lspconfig`, that is the only editor setup:

```lua
require("lspconfig").clangd.setup({
  cmd = {
    "clangd",
    "--load=/home/you/Projects/c-autotmpl/build/libc-autotmpl.so",
    "--background-index",
  },
})
```

Restart clangd after replacing the module. A verbose run should show lines like:

```text
[c-autotmpl] scanning /project, found 2 template(s)
[c-autotmpl] generated List(int) in /project/gen/list.h
[c-autotmpl] workspace scan complete
```

### Platform notes

Linux is the main development platform and uses the `.so` name from the
Makefile.

On macOS, build with the clang++ that produced your clangd. `dlopen` can load a
full path with a `.so` suffix. If your packaging expects `.dylib`, copy or
rename the built file. The architecture must match clangd.

On Windows the plugin needs a DLL built against a clangd with dynamic loading
support and the same MSVC ABI. The Makefile targets POSIX shared libraries and
has not been exercised for that case. The Windows CLI build in CI is supported.

### Tests

```sh
make selftest           # plugin core, no clangd process
make lsp-check          # live clangd with --load
make standalone-check   # CLI
make portability-check  # CLI linkage
make test               # all four
```

`clangd --check` does not run AST feature modules, so use `lsp-check` for the
plugin. To see plugin logs from that check:

```sh
C_AUTOTMPL_TEST_VERBOSE=1 make lsp-check
```

`make bench` builds a synthetic project and measures CLI and plugin overhead.

### When clangd changes

1. Check out the matching LLVM revision.
2. Rebuild clangd and `llvm-config`.
3. Rebuild the module with the same `LLVM_SRC`, `LLVM_BUILD`, `CXX`, and
   `LLVM_ABI`.
4. Run `make selftest` and `make lsp-check`.
5. Restart the editor.

Once distributions ship the clangd module headers, steps 1 and 2 should go away.
You should only need the system clangd development package and a rebuild of this
small module.

### Build errors

| Error | Fix |
| --- | --- |
| `FeatureModule.h` not found | Point `LLVM_SRC` and `LLVM_BUILD` at the tree that built clangd. |
| Undefined symbol at startup | Rebuild with the matching clangd revision, ABI, compiler, and flags. |
| `clang++` ABI errors | Check how clangd was built and set `LLVM_ABI` accordingly. |
| Wrong architecture on macOS | Build with the same arch as clangd. |
| Plugin loads but logs nothing | Use an absolute `--load` path and restart clangd. |

More runtime problems are covered in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
