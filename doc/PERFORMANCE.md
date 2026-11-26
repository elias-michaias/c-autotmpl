# Performance

c-autotmpl is designed to stay out of the way of a C build. The standalone CLI
is a single portable C99 executable: on the benchmark workload it adds roughly
20 ms to a one-second syntax check, and the clangd plugin spends about 2 ms per
edit. This page explains what is measured, how to reproduce it, and how the
configuration knobs change the cost.

## Run the benchmark

```sh
make bench          # full run: CLI, compile, cold generation, clangd plugin
make bench-quick    # fewer samples, useful as a smoke test
```

The script can also be called directly:

```sh
python3 tools/bench.py build/autotmpl build/libc-autotmpl.so \
  --files 80 --headers 500 --reps 15 --cold-reps 10 --warm-reps 8
```

Options:

| Flag | Default | Effect |
| --- | ---: | --- |
| `--files N` | 80 | Secondary source files in the synthetic project. |
| `--headers N` | 500 | Unrelated guarded headers, to make scanning cost visible. |
| `--reps N` | 15 | CLI and compile samples per case. |
| `--cold-reps N` | 10 | Cold-generation samples and clangd cold-open sessions. |
| `--warm-reps N` | 8 | clangd `didChange` samples per session. |
| `--warmup N` | 3 | Untimed warmup passes before sampling. |
| `--timeout N` | 30 | Seconds to wait for clangd diagnostics. |
| `--quick` | off | Shorthand for 5/3/3 samples and one warmup. |
| `--no-lsp` | off | Skip clangd measurements. |

Environment overrides: `CC` selects the preprocessor command used by
`preprocess=1`, `CLANG` selects the compiler for `-fsyntax-only`, and `CLANGD`
selects the clangd binary. All ambient `C_AUTOTMPL_*` variables are removed
while benchmarking so `autotmpl.conf` is the only configuration source.

## Workload

The generated project contains:

- 82 source files (one `main.c`, 80 secondary files, and one macro-only file);
- 512 headers, most of them unrelated guarded headers;
- 8 guarded user types;
- 160 direct `Vec(Type)`/`Vec_push(Type)` usages in `main.c`, plus 8 per
  secondary file;
- one `stc_vec(cstr)` usage;
- one usage spelled only through `#define MAKE_VEC(T) Vec(T)`.

The `Vec` template has a signature-defined operation, `Vec_push(T)`, so the
benchmark exercises dispatch generation as well as plain type instantiation.
The STC template exercises `profile`/`stc_auto` configuration. The macro-only
file exercises opt-in preprocessing without changing the default raw-text
workload.

## Methodology

- Each case is warmed up, then sampled with shuffled ordering on every pass.
  This prevents a slow period late in the run from being charged to only one
  configuration.
- Results show median with p10/p90 bounds. Numbers are wall-clock milliseconds
  around the command or LSP request.
- Generated state is reset between cold runs. Dry-run configurations reuse the
  fully generated state from setup, so they measure scanning, not writing.
- Plugin timing comes from `C_AUTOTMPL_TIMING=1` logs. Each clangd session is a
  separate process with its own generated state.
- The clangd columns measure time to first diagnostics, not a full link or a
  complete AST build. Use them as relative comparisons, not absolute editor
  latency promises.
- Omitting `preprocess=1` never invokes an external compiler. That is the main
  performance cliff in the results below.

## Configuration matrix

| Case | Configuration | What it shows |
| --- | --- | --- |
| raw | defaults | Lexical scan of unexpanded source. |
| dispatch=generic | `dispatch=1` | `_Generic` call generation from signature-defined operations. |
| dispatch=builtin | `dispatch=1`, `dispatch_backend=builtin` | Compiler-builtin dispatch backend. |
| dispatch=postfix | `dispatch=1`, `dispatch_style=postfix`, `dispatch_affix=_method` | Affix parsing and naming; no spelling penalty. |
| stc=explicit | `profile=stc`, `stc_templates=stc_vec` | Explicit STC wrapper selection. |
| stc=auto | `profile=stc`, `stc_auto=1` | STC detection from template-header contents. |
| preprocess | `preprocess=1` | Full `cc -E -P` expansion for macro-only usages. |
| preprocess+dispatch | `preprocess=1`, `dispatch=1` | Both expensive expansion and dispatch generation. |

## Representative results

Measured on the development machine with the default sample counts. Your
numbers will differ; the ratios are the useful part.

### Steady-state compile

| Measurement | Median | p10 | p90 |
| --- | ---: | ---: | ---: |
| `clang -fsyntax-only` | 996 ms | 986 ms | 1001 ms |
| `clang + autotmpl --dry-run` | 1020 ms | 1000 ms | 1033 ms |
| Added pre-build time | +23 ms (+2.4%) | | |

The CLI scan is tiny next to compilation. Even with a stale generated header,
a no-op dry run costs about 2% of the compile in this workload.

### CLI dry-run by configuration

| Configuration | Median | p10 | p90 | vs raw |
| --- | ---: | ---: | ---: | ---: |
| raw | 20.37 ms | 19.76 | 20.75 | — |
| dispatch=generic | 20.55 ms | 20.19 | 21.11 | +0.9% |
| dispatch=builtin | 20.78 ms | 20.45 | 21.22 | +2.0% |
| dispatch=postfix | 20.25 ms | 19.66 | 20.48 | -0.6% |
| stc=explicit | 17.48 ms | 17.04 | 17.94 | -14.2% |
| stc=auto | 17.42 ms | 17.08 | 17.76 | -14.5% |
| preprocess | 598.34 ms | 583.99 | 623.43 | +2837% |
| preprocess+dispatch | 602.77 ms | 591.86 | 617.33 | +2859% |

Dispatch backend and affix style are effectively free here. STC cases are
faster in this particular workload because the preset skips guard lookups for
STC-owned types; that is workload-specific rather than a general guarantee.
Preprocessing dominates because it runs one compiler process per source file.

### Cold generation

| Configuration | Median | p10 | p90 |
| --- | --- | ---: | ---: |
| raw | 20.59 ms | 20.32 | 20.87 |
| dispatch | 20.75 ms | 20.47 | 21.04 |
| preprocess bootstrap | 549.57 ms | 543.11 | 553.59 |
| stc=auto | 17.62 ms | 17.27 | 17.76 |

Cold raw generation is still around 20 ms. The preprocess bootstrap case also
creates and removes empty `gen/*.h` stubs, which is negligible next to the
preprocessor invocations.

### Clangd plugin

Time to first diagnostics:

| Measurement | Median | Samples |
| --- | ---: | ---: |
| cold parse, no plugin | 30.26 ms | 9 |
| cold parse, plugin default | 40.23 ms | 9 |
| cold parse, plugin dispatch | 40.26 ms | 9 |
| cold parse, plugin stc+dispatch | 40.31 ms | 9 |
| warm parse, no plugin | 60.36 ms | 80 |
| warm parse, plugin default | 60.34 ms | 80 |
| warm parse, plugin dispatch | 60.34 ms | 80 |
| warm parse, plugin stc+dispatch | 60.46 ms | 80 |

Cold opens pay about 10 ms for workspace scanning and first generation. Warm
edits are indistinguishable from no-plugin parsing in this run.

Plugin-internal phases for the default configuration:

| Phase | Median | Samples |
| --- | ---: | ---: |
| registry scan | 0.12 ms | 10 |
| `prepareUpdate`, first open | 9.81 ms | 10 |
| `prepareUpdate`, edits | 1.85 ms | 80 |
| — scan | 0.00 ms | 90 |
| — usages | 1.16 ms | 90 |
| — prune | 0.03 ms | 90 |
| — generate | 0.66 ms | 90 |

Enabling dispatch raised warm `prepareUpdate` from 1.85 ms to 2.07 ms.
`stc+dispatch` was the same at 2.07 ms. Both are still far below editor input
latency.

## Practical guidance

- Leave `preprocess=1` off unless a usage exists only through macro expansion.
  It is exact and opt-in, but costs one external compiler invocation per file.
- `dispatch=1` and its style/backend variants are cheap. Choose them for
  readability and API shape, not for performance reasons.
- STC profiles can be faster or slower depending on how many guarded user types
  the scanner would otherwise inspect. Measure your own project.
- The plugin's first-open cost is mostly workspace discovery. Later edits only
  rescan the open buffer and regenerate changed blocks.
- Use `--files`, `--headers`, and `--reps` to model your project. Add more
  samples before trusting sub-millisecond differences.
