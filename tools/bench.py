#!/usr/bin/env python3
"""Benchmark the time added by c-autotmpl.

The benchmark separates the three ways c-autotmpl can affect a build:

  * steady-state compilation of already-generated C code;
  * a standalone `autotmpl` pre-build step, both no-op and cold generation;
  * clangd parse latency with and without the clangd feature module loaded.

Usage:
  python3 tools/bench.py [autotmpl] [plugin.so] [--files N] [--headers N] [--reps N]
"""

import glob
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

DEFAULT_FILES = 80
DEFAULT_HEADERS = 500
TYPES = 8
USAGES_PER_MAIN = 80
USAGES_PER_FILE = 8

BASELINE_GEN = """\
#ifndef GEN_VEC_H
#define GEN_VEC_H

#define T int
#include "../tmpl/vec.h"
#undef T

#endif
"""


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def run(cmd, cwd, capture=False):
    return subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
        stderr=subprocess.PIPE if capture else subprocess.DEVNULL,
        text=True,
    )


def require_ok(proc, cmd):
    if proc.returncode != 0:
        detail = ""
        if proc.stderr:
            detail = "\n" + proc.stderr[-4000:]
        raise RuntimeError("command failed: " + " ".join(cmd) + detail)


def bench_command(cmd, cwd, reps, warmup=2):
    for _ in range(warmup):
        require_ok(run(cmd, cwd), cmd)
    samples = []
    for _ in range(reps):
        start = time.perf_counter()
        proc = run(cmd, cwd)
        samples.append((time.perf_counter() - start) * 1000)
        require_ok(proc, cmd)
    return samples


def bench_pair(first, second, cwd, reps, warmup=2):
    for _ in range(warmup):
        require_ok(run(first, cwd), first)
        require_ok(run(second, cwd), second)
    samples = []
    for _ in range(reps):
        start = time.perf_counter()
        proc = run(first, cwd)
        require_ok(proc, first)
        proc = run(second, cwd)
        require_ok(proc, second)
        samples.append((time.perf_counter() - start) * 1000)
    return samples


def bench_cold_generation(cli, root, files, gen_path, baseline, reps,
                          warmup=2):
    cmd = [cli, "--root", root] + files
    for _ in range(warmup):
        write(gen_path, baseline)
        require_ok(run(cmd, root), cmd)
    samples = []
    for _ in range(reps):
        write(gen_path, baseline)
        start = time.perf_counter()
        proc = run(cmd, root)
        samples.append((time.perf_counter() - start) * 1000)
        require_ok(proc, cmd)
    return samples


def stats(samples):
    return {
        "median": statistics.median(samples),
        "min": min(samples),
        "max": max(samples),
    }


def format_ms(label, s):
    print(f"{label:<44} {s['median']:8.2f} ms  "
          f"(min {s['min']:7.2f}, max {s['max']:7.2f})")


def format_overhead(label, value, base, suffix=""):
    pct = 100.0 * value / base if base else 0.0
    print(f"{label:<44} {value:+8.2f} ms  ({pct:+6.1f}%{suffix})")


def make_project(root, file_count, header_count):
    os.makedirs(os.path.join(root, "tmpl"))
    os.makedirs(os.path.join(root, "gen"))
    os.makedirs(os.path.join(root, "src"))
    os.makedirs(os.path.join(root, "extra"))

    write(os.path.join(root, ".clangd"),
          'CompileFlags:\n  Add: ["-std=c23"]\n')
    write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template Vec(T)
#define Vec(T) Vec_##T
#define CAT_IMPL_(A, B) A##B
#define CAT_(A, B) CAT_IMPL_(A, B)
typedef struct { T value; } CAT_(Vec_, T);
""")

    includes = []
    for i in range(TYPES):
        name = f"Type{i}"
        guard = f"TYPE{i}_H"
        includes.append(f'#include "type{i}.h"')
        write(os.path.join(root, f"type{i}.h"), f"""\
#ifndef {guard}
#define {guard}
typedef struct {{ int x; }} {name};
#endif
""")
    # Unrelated guarded headers make registry/type-index scan cost visible.
    for i in range(header_count):
        guard = f"EXTRA{i}_H"
        write(os.path.join(root, "extra", f"h{i}.h"), f"""\
#ifndef {guard}
#define {guard}
typedef struct {{ int x; }} Extra{i};
#endif
""")
    include_block = "\n".join(includes)
    gen_path = os.path.join(root, "gen", "vec.h")
    write(gen_path, BASELINE_GEN)

    def usages(n, offset):
        return "\n".join(
            f"  Vec(Type{(offset + i) % TYPES}) v{i};"
            for i in range(n)
        )

    main = f"""\
{include_block}
#include "gen/vec.h"

int main(void) {{
{usages(USAGES_PER_MAIN, 0)}
  return 0;
}}
"""
    write(os.path.join(root, "main.c"), main)

    files = [os.path.join(root, "main.c")]
    for n in range(file_count):
        path = os.path.join(root, "src", f"f{n}.c")
        nested_includes = "\n".join(
            line.replace('"type', '"../type') for line in includes
        )
        write(path, f"""\
{nested_includes}
#include "../gen/vec.h"

int f{n}(void) {{
{usages(USAGES_PER_FILE, n)}
  return 0;
}}
""")
        files.append(path)
    return files, gen_path


def send(proc, message):
    body = json_dumps(message)
    proc.stdin.write(
        b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
    )
    proc.stdin.flush()


def json_dumps(message):
    # Imported lazily to keep the top-level dependency list small.
    import json
    return json.dumps(message).encode()


def count_messages(path, needle):
    try:
        with open(path, errors="replace") as f:
            return f.read().count(needle)
    except OSError:
        return 0


def wait_for_message(path, needle, before, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if count_messages(path, needle) > before:
            return
        time.sleep(0.01)
    raise RuntimeError("timed out waiting for clangd diagnostics ("
                       + needle + ")")


def plugin_log_text(root, pattern):
    text = ""
    for path in glob.glob(os.path.join(root, pattern)):
        try:
            with open(path, errors="replace") as f:
                text += f.read()
        except OSError:
            pass
    return text


def plugin_scan_times(root):
    text = plugin_log_text(root, "clangd-plugin*.log")
    return [float(m) for m in re.findall(
        r"\[c-autotmpl\] scanning .* in ([0-9.]+) ms", text
    )]


def plugin_prepare_samples(root):
    """Returns (cold total, warm total, phase samples) from plugin logs."""
    pattern = re.compile(
        r"\[c-autotmpl\] prepareUpdate in ([0-9.]+) ms "
        r"\(scan ([0-9.]+), usages ([0-9.]+), prune ([0-9.]+), "
        r"generate ([0-9.]+)\)"
    )
    cold, warm = [], []
    phases = {"scan": [], "usages": [], "prune": [], "generate": []}
    for path in glob.glob(os.path.join(root, "clangd-plugin*.log")):
        try:
            with open(path, errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        matches = pattern.findall(text)
        if not matches:
            continue
        cold.append(float(matches[0][0]))
        warm.extend(float(m[0]) for m in matches[1:])
        for m in matches:
            for key, value in zip(phases, m[1:]):
                phases[key].append(float(value))
    return cold, warm, phases


def verify_plugin_load(root, pattern, expect_loaded):
    text = plugin_log_text(root, pattern)
    loaded = "[c-autotmpl]" in text
    if expect_loaded and not loaded:
        raise RuntimeError("clangd did not load the feature module")
    if not expect_loaded and loaded:
        raise RuntimeError("plugin loaded in the no-plugin baseline")


def lsp_parse_samples(root, main_file, plugin, clangd, cold_reps, warm_reps):
    cmd = [clangd, "--log=error"]
    if plugin:
        cmd.append("--load=" + plugin)
    uri = "file://" + main_file
    diagnostic_needle = '"uri":"' + uri + '"'
    samples = []

    for cold in range(cold_reps):
        tag = ("plugin" if plugin else "base") + str(cold)
        stdout_path = os.path.join(root, "clangd-" + tag + ".stdout")
        log_path = os.path.join(root, "clangd-" + tag + ".log")
        env = os.environ.copy()
        if plugin:
            env["C_AUTOTMPL_TIMING"] = "1"
        with open(stdout_path, "wb") as stdout, open(log_path, "wb") as log:
            proc = subprocess.Popen(
                cmd,
                cwd=root,
                env=env,
                stdin=subprocess.PIPE,
                stdout=stdout,
                stderr=log,
            )
            try:
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "processId": None,
                        "rootUri": "file://" + root,
                        "capabilities": {},
                    },
                })
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "initialized",
                    "params": {},
                })

                with open(main_file) as f:
                    text = f.read()

                before = count_messages(stdout_path, diagnostic_needle)
                start = time.perf_counter()
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {
                        "uri": uri,
                        "languageId": "c",
                        "version": 1,
                        "text": text,
                    }},
                })
                wait_for_message(stdout_path, diagnostic_needle, before, 30)
                elapsed = (time.perf_counter() - start) * 1000
                if cold > 0:
                    samples.append(("cold", elapsed))

                for i in range(warm_reps):
                    before = count_messages(stdout_path, diagnostic_needle)
                    start = time.perf_counter()
                    send(proc, {
                        "jsonrpc": "2.0",
                        "method": "textDocument/didChange",
                        "params": {
                            "textDocument": {"uri": uri, "version": i + 2},
                            "contentChanges": [{
                                "text": text + " " * (i + 1),
                            }],
                        },
                    })
                    wait_for_message(stdout_path, diagnostic_needle, before, 30)
                    elapsed = (time.perf_counter() - start) * 1000
                    samples.append(("warm", elapsed))
            finally:
                try:
                    send(proc, {
                        "jsonrpc": "2.0",
                        "id": 2,
                        "method": "shutdown",
                        "params": None,
                    })
                    send(proc, {
                        "jsonrpc": "2.0",
                        "method": "exit",
                        "params": None,
                    })
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    return samples


def main():
    cli = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/autotmpl")
    plugin = os.path.abspath(
        sys.argv[2] if len(sys.argv) > 2 else "build/libc-autotmpl.so"
    )
    files = DEFAULT_FILES
    headers = DEFAULT_HEADERS
    reps = 5
    if "--files" in sys.argv:
        files = int(sys.argv[sys.argv.index("--files") + 1])
    if "--headers" in sys.argv:
        headers = int(sys.argv[sys.argv.index("--headers") + 1])
    if "--reps" in sys.argv:
        reps = int(sys.argv[sys.argv.index("--reps") + 1])

    clang = os.environ.get("CLANG") or shutil.which("clang") or \
        "/home/elias/.local/build/llvm-main-plugin/bin/clang"
    clangd = os.environ.get("CLANGD") or shutil.which("clangd") or "clangd"
    for path in (cli, plugin, clang):
        if not os.path.exists(path):
            print("missing executable: " + path, file=sys.stderr)
            return 1
    if not shutil.which(clangd) and not os.path.exists(clangd):
        print("missing executable: " + clangd, file=sys.stderr)
        return 1

    root = tempfile.mkdtemp(prefix="c-autotmpl-bench-")
    try:
        sources, gen_path = make_project(root, files, headers)

        setup = run([cli, "--root", root] + sources, root, capture=True)
        require_ok(setup, [cli])
        generated = open(gen_path).read()
        check = run([clang, "-fsyntax-only", "-std=c23",
                     os.path.join(root, "main.c")] + sources[1:],
                    root, capture=True)
        require_ok(check, [clang, "-fsyntax-only"])

        dry_cmd = [cli, "--root", root, "--dry-run"] + sources
        compile_cmd = [clang, "-fsyntax-only", "-std=c23",
                       os.path.join(root, "main.c")] + sources[1:]
        compile_stats = stats(bench_command(compile_cmd, root, reps))
        dry_stats = stats(bench_command(dry_cmd, root, reps))
        both_stats = stats(bench_pair(dry_cmd, compile_cmd, root, reps))
        cold_gen_stats = stats(bench_cold_generation(
            cli, root, sources, gen_path, BASELINE_GEN, reps
        ))
        write(gen_path, generated)

        main_file = os.path.join(root, "main.c")
        no_plugin = lsp_parse_samples(root, main_file, None, clangd,
                                      reps + 1, reps)
        with_plugin = lsp_parse_samples(root, main_file, plugin, clangd,
                                        reps + 1, reps)
        verify_plugin_load(root, "clangd-base*.log", expect_loaded=False)
        verify_plugin_load(root, "clangd-plugin*.log", expect_loaded=True)
        scan_times = plugin_scan_times(root)

        def split(samples, kind):
            return stats([t for k, t in samples if k == kind])

        print(f"project: {files + 1} sources, {headers + TYPES + 2} headers, "
              f"{TYPES} guarded types, {USAGES_PER_MAIN} usages in main.c")
        print("")
        format_ms("clang -fsyntax-only", compile_stats)
        format_ms("autotmpl --dry-run (no-op)", dry_stats)
        format_ms("autotmpl --dry-run + clang", both_stats)
        format_overhead("  added pre-build time",
                        both_stats["median"] - compile_stats["median"],
                        compile_stats["median"], " of compile")
        format_ms("autotmpl cold generation", cold_gen_stats)
        print("")

        for kind in ("cold", "warm"):
            base = split(no_plugin, kind)
            with_mod = split(with_plugin, kind)
            format_ms(f"clangd {kind} parse, no plugin", base)
            format_ms(f"clangd {kind} parse, plugin", with_mod)
            delta = with_mod["median"] - base["median"]
            pct = 100.0 * delta / base["median"] if base["median"] else 0.0
            print(f"{'  plugin overhead':<44} {delta:+8.2f} ms  ({pct:+6.1f}%)")
        if scan_times:
            scan_stats = stats(scan_times)
            format_ms("c-autotmpl registry scan (in-process)", scan_stats)
        prepare_cold, prepare_warm, phases = plugin_prepare_samples(root)
        if prepare_cold:
            format_ms("c-autotmpl prepareUpdate (first open)",
                      stats(prepare_cold))
        if prepare_warm:
            format_ms("c-autotmpl prepareUpdate (edits)",
                      stats(prepare_warm))
        for name in ("scan", "usages", "prune", "generate"):
            if phases[name]:
                format_ms("  " + name, stats(phases[name]))
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
