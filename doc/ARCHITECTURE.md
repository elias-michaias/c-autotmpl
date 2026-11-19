# Architecture

This document describes the shared policy and the two pipelines that implement
it. For build details, start with [BUILD.md](BUILD.md).

## Shared concepts

### Template definition

A decorator in a header:

```c
// -- template List(T)
// -- template type Box(T)
```

produces a `TemplateDef` with:

- `Name` — `List`;
- `Params` — `T` (first parameter is used as the re-include macro parameter);
- `Header` — absolute path of the decorated header;
- `GeneratedHeader` — sibling `gen/list.h` or `gen/box.h`;
- `IncludeFromGenDir` — relative path back to the decorated header, e.g.
  `../tmpl/list.h`.

A directory named `tmpl/` or `templates/` maps one level up: `tmpl/list.h`
becomes `gen/list.h`. Other layouts get a sibling `gen/` in the template
header's own directory. Template names are lowercased for the generated
filename.

### Usage and mangling

Both implementations find:

- direct applications such as `List(int)`;
- already-mangled spellings such as `List_int`;
- nested applications such as `List(Box(int))`.

Nested applications are canonicalized inside-out: `Box(int)` becomes `Box_int`,
then the outer usage becomes `List(Box_int)`. The usage list records
dependencies so inner blocks can be emitted before the block that needs them.
No call-site alias macros are generated.

### Generated block

Each instantiation is a marked block:

```c
/* c-autotmpl: List(float) */
#ifndef C_AUTOTMPL_LIST_FLOAT_...
#ifdef FLOAT_GUARD_IF_ANY
#define C_AUTOTMPL_LIST_FLOAT_...
#define T float
#include "../tmpl/list.h"
#undef T
#endif
#endif
```

The marker is used for idempotency and pruning. The `C_AUTOTMPL_...` macro is a
presence macro: the same nested instantiation may be mirrored into more than one
generated header, and whichever copy is seen first wins. The presence macro is
defined only **after** all applicable guards pass, so a skipped copy does not
suppress a usable fallback elsewhere.

### Guards

- Builtin and fixed-width scalar types are emitted without a type guard.
- User-defined types need a traditional include guard that generated code can
  test with `#ifdef`.
- Nested dependencies add the generated dependency header's file guard, e.g.
  `GEN_BOX_H`, around the **entire** block.
- `#pragma once` cannot be tested and is skipped with a one-time warning.

See [TEMPLATES.md](TEMPLATES.md) for the author-side rules.

---

## Plugin pipeline

The clangd module is registered with
`FeatureModuleRegistry::Add<AutotmplModule>`. It wraps clangd's
`textDocument/didOpen`, `didChange`, and `didClose` notifications and installs
an `ASTListener`.

```text
didOpen / didChange (unsaved text)
        |
        v
SharedState::prepareUpdate
        |
        +-- ensureScanned(MainFile) ....... root + template registry (once)
        |
        +-- record OpenFiles / UsageCache . markers wanted per generated header
        |
        +-- startWorkspaceScan() .......... background thread, once
        |
        +-- ensureTypesScanned(Text) ...... lazy guard index for used types
        |
        +-- pruneAll() .................... only after workspace scan completes
        |
        +-- generateFromText(Text) ........ write missing blocks now
        |
        v
clangd parses the file with the updated headers on disk
```

### Important properties

- **Unsaved buffers win.** Generation happens in the LSP notification, before
  clangd builds the preamble. Disk content for open files is not used for
  generation.
- **Root discovery is two-pass.** It first looks for a `tmpl/` or `templates/`
  directory containing a decorator while walking up to eight parents. Then it
  falls back to `.clangd`, `compile_commands.json`, or `.git`. `C_AUTOTMPL_ROOT`
  overrides both passes.
- **Template scanning is cached.** Registry scanning happens once per clangd
  process.
- **Type guards are lazy.** Only user-defined type names seen in actual usages
  are indexed. If a later open file introduces a new type, the index is
  extended.
- **AST fallback exists.** The text index is tried first; if it has no guarded
  declaration, the plugin asks the current AST for candidate declaring headers
  and reads their guards. This helps with types declared via headers the text
  scanner would miss.
- **Pruning is deferred.** A background thread performs the first workspace
  usage scan and refreshes it periodically. Before `workspace scan complete`
  appears in the log, the module generates but does not prune.
- **External edits are respected.** Generated-header content is cached with an
  mtime/size stamp. If the file changes outside the plugin, the cache is
  reloaded instead of overwriting the user's edit.
- **Manual blocks count.** Hand-written `#define T ... / #include / #undef T`
  blocks are recognized so migration does not create duplicate definitions.

### AST listener role

The listener's `sawDiagnostic` hook is a fallback for undeclared-name
diagnostics whose spelling is visible in the AST but was not found by the text
scan (for example, some macro-expanded usages). It extracts the source spelling,
expands nested applications, generates the chain, and can attach an include-fix
for the generated header. Normal interactive generation does not depend on this
hook.

### Synchronization

`SharedState` owns the registry, type index, instantiator, open-document cache,
usage cache, and workspace thread. Separate mutexes protect scanning, type
lookups, guard lookups, and usage snapshots. AST worker threads can call into
the same state; mutations are serialized and snapshots are taken before writes.

---

## CLI pipeline

The CLI is a lexer, not a C parser:

```text
input files/directories
        |
        +-- collect source files (deterministic, files before dirs)
        +-- determine root: --root, or common ancestor of inputs
        +-- registry_scan(root) ........ decorator comments -> TemplateDef list
        +-- scan_usages(file) ........... Name(Type) / Name_Type / nested chains
        +-- type_index_scan(root, only-wanted-names)
        |
        v
emit_usage(...) for each top-level chain
        |
        +-- write missing blocks into generated headers (or --dry-run)
```

Differences from the plugin:

| Behavior | Plugin | CLI |
| --- | --- | --- |
| Unsaved buffers | Yes, from LSP notifications | No, reads files on disk |
| Root discovery | Decorator-first upward walk, then build markers, then env override | `--root`, otherwise common ancestor of inputs |
| Type guard lookup | Text index plus clang AST fallback | Text index only |
| Pruning | Yes, after workspace scan | No; the CLI is additive and idempotent |
| Include-fix suggestions | Diagnostic fallback can offer one | No |
| Guard cache reload | mtime/size aware across edits | Cached within one process run |

The CLI's job is to be cheap and reproducible in scripts. Run it from a build
system before compiling; use the plugin for interactive feedback while writing
code. Both paths produce the same block text for the same inputs, which is
enforced by the checks in `tools/`.

---

## Pruning semantics

The plugin tracks, per source file, the set of instantiation markers it wants
in each generated header. Open files use their unsaved text; other files are
re-read when their mtime or size changes.

For nested chains, markers are recorded both in the nested template's own
generated header and in the outermost generated header, because deep chains are
flattened into the outermost file. Pruning therefore removes mirrored copies
consistently.

Pruning operates on marker-delimited blocks and also recognizes hand-written
blocks. It never removes unrelated content from `gen/*.h`.

---

## Performance notes

- The CLI typically runs in ~1–2 ms on projects with thousands of unrelated
  headers. It uses a template-directory fast path, caches generated-header
  contents within a run, indexes only guards for types actually used, and stops
  the type-index walk once all wanted names are found.
- The plugin's first `prepareUpdate` is a few tenths of a millisecond in the
  benchmark project. Workspace scanning runs off the LSP thread so keystrokes
  do not wait on unrelated files.
- See `make bench` and [TESTING.md](TESTING.md) for measurements.
