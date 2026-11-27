# Performance

c-autotmpl is designed to stay out of the way of a C build. The standalone CLI
is a single portable C99 executable: on the benchmark workload it adds roughly
20 ms to a one-second syntax check, and the clangd plugin spends about 2 ms per
edit. The numbers below show what is measured and how the configuration knobs
change the cost.

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
file exercises both preprocessing backends without changing the default raw-text
workload: the `full` backend expands all 82 sources in one batched compiler
invocation, while `local` expands wrapper macros inside the CLI. See
[Generic calls](USAGE.md#generic-calls) for the dispatch selectors written to
`gen/dispatch.h`.

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
- Omitting `preprocess=1` never invokes an external compiler. With the `local`
  backend, enabling it still does not start a subprocess.
- With `preprocess_backend=full`, sources are grouped into compiler invocations
  capped at 128 files and by command-line length. Macros remain isolated per
  input because each file is still a separate preprocessing translation unit.
- The `local` backend collects macros per input translation unit, following
  local includes but not evaluating conditionals. It is much faster but less
  faithful than `full`.

## Configuration matrix

| Case | Configuration | What it shows |
| --- | --- | --- |
| raw | defaults | Lexical scan of unexpanded source. |
| dispatch=generic | `dispatch=1` | `_Generic` call generation from signature-defined operations. |
| dispatch=builtin | `dispatch=1`, `dispatch_backend=builtin` | Compiler-builtin dispatch backend. |
| dispatch=postfix | `dispatch=1`, `dispatch_style=postfix`, `dispatch_affix=_method` | Affix parsing and naming; no spelling penalty. |
| dispatch=truncate | `dispatch=1`, `dispatch_truncate=1` | Bare public macros and direct `<op>_<args>` targets; same selector generation. |
| stc=explicit | `profile=stc`, `stc_templates=stc_vec` | Explicit STC wrapper selection. |
| stc=auto | `profile=stc`, `stc_auto=1` | STC detection from template-header contents. |
| preprocess | `preprocess=1` | Default `full` backend: batched `cc -E -P` expansion. |
| preprocess+dispatch | `preprocess=1`, `dispatch=1` | Batched expansion plus dispatch generation. |
| preprocess=local | `preprocess=1`, `preprocess_backend=local` | Internal wrapper-macro expansion with no subprocess. |
| preprocess=local+dispatch | `preprocess=1`, `preprocess_backend=local`, `dispatch=1` | Local expansion plus dispatch generation. |

## Representative results

Measured on the development machine with the default sample counts. Your
numbers will differ; the ratios are the useful part.

### Steady-state compile

| Measurement | Median | p10 | p90 |
| --- | ---: | ---: | ---: |
| `clang -fsyntax-only` | 998 ms | 984 ms | 1009 ms |
| `clang + autotmpl --dry-run` | 1020 ms | 1004 ms | 1024 ms |
| Added pre-build time | +22 ms (+2.2%) | | |

The CLI scan is tiny next to compilation. Even with a stale generated header,
a no-op dry run costs about 2% of the compile in this workload.

### CLI dry-run by configuration

| Configuration | Median | p10 | p90 | vs raw |
| --- | ---: | ---: | ---: | ---: |
| raw | 20.25 ms | 19.99 | 20.55 | — |
| dispatch=generic | 20.72 ms | 20.36 | 21.08 | +2.3% |
| dispatch=builtin | 20.91 ms | 20.64 | 21.07 | +3.3% |
| dispatch=postfix | 20.20 ms | 19.96 | 20.38 | -0.2% |
| stc=explicit | 17.35 ms | 17.06 | 17.61 | -14.3% |
| stc=auto | 17.41 ms | 17.12 | 17.67 | -14.0% |
| preprocess | 429.11 ms | 418.43 | 432.95 | +2019% |
| preprocess+dispatch | 423.50 ms | 415.32 | 433.15 | +1991% |
| preprocess=local | 39.23 ms | 38.83 | 39.50 | +94% |
| preprocess=local+dispatch | 39.50 ms | 39.01 | 39.85 | +95% |

Dispatch backend and affix style are effectively free here. STC cases are
faster in this particular workload because the preset skips guard lookups for
STC-owned types; that is workload-specific rather than a general guarantee.
The `full` backend is still expensive because every include must be expanded,
though batching removed repeated compiler startup: the same workload dropped
from about 598 ms with a per-file loop to about 429 ms here. The `local` backend
cuts that further to about 39 ms, roughly 11× faster than `full` and only about
19 ms above a raw scan.

### Cold generation

| Configuration | Median | p10 | p90 |
| --- | --- | ---: | ---: |
| raw | 20.68 ms | 20.33 | 21.14 |
| dispatch | 20.81 ms | 20.55 | 21.28 |
| preprocess bootstrap | 364.51 ms | 357.84 | 373.13 |
| preprocess local | 25.29 ms | 25.01 | 25.38 |
| stc=auto | 17.63 ms | 17.41 | 18.06 |

Cold raw generation is still around 20 ms. The `full` bootstrap case also
creates and removes empty `gen/*.h` stubs, which is negligible next to the
single batched preprocessor invocation. The `local` backend needs no stubs and
adds only about 5 ms over cold raw generation here.

### clangd plugin

Time to first diagnostics:

| Measurement | Median | Samples |
| --- | ---: | ---: |
| cold parse, no plugin | 30.24 ms | 9 |
| cold parse, plugin default | 40.28 ms | 9 |
| cold parse, plugin dispatch | 40.34 ms | 9 |
| cold parse, plugin stc+dispatch | 40.31 ms | 9 |
| warm parse, no plugin | 60.43 ms | 80 |
| warm parse, plugin default | 60.39 ms | 80 |
| warm parse, plugin dispatch | 60.46 ms | 80 |
| warm parse, plugin stc+dispatch | 60.44 ms | 80 |

Cold opens pay about 10 ms for workspace scanning and first generation. Warm
edits are indistinguishable from no-plugin parsing in this run.

Plugin-internal phases for the default configuration:

| Phase | Median | Samples |
| --- | ---: | ---: |
| registry scan | 0.12 ms | 10 |
| `prepareUpdate`, first open | 9.77 ms | 10 |
| `prepareUpdate`, edits | 1.84 ms | 80 |
| — scan | 0.00 ms | 90 |
| — usages | 1.16 ms | 90 |
| — prune | 0.02 ms | 90 |
| — generate | 0.65 ms | 90 |

Enabling dispatch raised warm `prepareUpdate` from 1.84 ms to 2.08 ms.
`stc+dispatch` was the same at 2.07 ms. Both are still far below editor input
latency.

## Practical guidance

- Leave `preprocess=1` off unless a usage exists only through macro expansion.
  Prefer `preprocess_backend=local` for ordinary wrapper macros; it starts no
  subprocess and costs about twice a raw scan here. Use `full` only when the
  usage depends on conditionals, system headers, predefined macros, or other
  compiler-specific expansion.
- `dispatch=1` and its style/backend variants are cheap. Choose them for
  readability and API shape, not for performance reasons. `dispatch_truncate=1`
  changes only the public spelling; it does not change selector cost.
- STC profiles can be faster or slower depending on how many guarded user types
  the scanner would otherwise inspect. Measure your own project.
- The plugin's first-open cost is mostly workspace discovery. Later edits only
  rescan the open buffer and regenerate changed blocks.
- Use the [benchmark options](#run-the-benchmark) to model your project. Add
  more samples before trusting sub-millisecond differences.

---

[README](../README.md) &bull; [Build](BUILD.md) &bull; [Usage](USAGE.md) &bull; [Troubleshooting](TROUBLESHOOTING.md)
