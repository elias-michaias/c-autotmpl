#!/usr/bin/env python3
"""Benchmark the time added by c-autotmpl.

The benchmark separates the ways c-autotmpl can affect a build:

  * steady-state compilation of already-generated C code;
  * standalone `autotmpl --dry-run` scans under several configurations;
  * cold generation from an empty `gen/` tree, with and without preprocessing;
  * clangd parse latency with and without the feature module, and with
    different plugin configurations.

Samples are interleaved and reported as median/p10/p90 so configuration
comparisons are not distorted by machine noise.

Usage:
  python3 tools/bench.py [autotmpl] [plugin.so] [options]

Options:
  --files N        number of secondary source files (default: 80)
  --headers N      number of unrelated guarded headers (default: 500)
  --reps N         CLI/compile samples per case (default: 15)
  --cold-reps N    cold generation and clangd cold samples (default: 10)
  --warm-reps N    clangd edit samples per session (default: 8)
  --warmup N       untimed warmup passes (default: 3)
  --timeout N      seconds to wait for clangd diagnostics (default: 30)
  --quick          smaller sample counts for a fast smoke run
  --no-lsp         skip clangd measurements
"""

import glob
import os
import random
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

DEFAULT_FILES = 80
DEFAULT_HEADERS = 500
DEFAULT_REPS = 15
DEFAULT_COLD_REPS = 10
DEFAULT_WARM_REPS = 8
DEFAULT_WARMUP = 3
DEFAULT_TIMEOUT = 30

TYPES = 8
USAGES_PER_MAIN = 80
USAGES_PER_FILE = 8

BASELINE_VEC_GEN = """\
#ifndef GEN_VEC_H
#define GEN_VEC_H
#endif
"""

BASELINE_STC_GEN = """\
#ifndef GEN_STC_VEC_H
#define GEN_STC_VEC_H
#endif
"""

CLI_SCENARIOS = (
    ("raw", ""),
    ("dispatch=generic", "dispatch=1\n"),
    ("dispatch=builtin", "dispatch=1\ndispatch_backend=builtin\n"),
    ("dispatch=postfix",
     "dispatch=1\ndispatch_style=postfix\ndispatch_affix=_method\n"),
    ("stc=explicit", "profile=stc\nstc_templates=stc_vec\n"),
    ("stc=auto", "profile=stc\nstc_auto=1\n"),
    ("preprocess", "preprocess=1\n"),
    ("preprocess+dispatch", "preprocess=1\ndispatch=1\n"),
)

COLD_SCENARIOS = (
    ("raw", ""),
    ("dispatch", "dispatch=1\n"),
    ("preprocess bootstrap", "preprocess=1\n"),
    ("stc=auto", "profile=stc\nstc_auto=1\n"),
)

PLUGIN_SCENARIOS = (
    ("plugin-default", "default", ""),
    ("plugin-dispatch", "dispatch", "dispatch=1\n"),
    ("plugin-stc-dispatch", "stc+dispatch",
     "profile=stc\nstc_auto=1\ndispatch=1\n"),
)

CONFIG_ENV_PREFIX = "C_AUTOTMPL_"


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def run(cmd, cwd, capture=False, env=None):
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
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


def bench_env(**overrides):
    """Remove ambient c-autotmpl settings so autotmpl.conf is authoritative."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(CONFIG_ENV_PREFIX)}
    env.update(overrides)
    return env


def write_conf(root, text, preprocess_cc=None):
    if preprocess_cc and "preprocess=1" in text \
            and "preprocess_cc=" not in text:
        text += "preprocess_cc=%s\n" % shlex.quote(preprocess_cc)
    write(os.path.join(root, "autotmpl.conf"), text)


def write_state(root, state, missing=()):
    for path, content in state.items():
        if path in missing:
            if os.path.exists(path):
                os.remove(path)
        else:
            write(path, content)


def percentile(samples, fraction):
    ordered = sorted(samples)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * fraction
    low = int(pos)
    high = min(low + 1, len(ordered) - 1)
    weight = pos - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def stats(samples):
    return {
        "n": len(samples),
        "median": statistics.median(samples),
        "p10": percentile(samples, 0.10),
        "p90": percentile(samples, 0.90),
        "min": min(samples),
        "max": max(samples),
        "stdev": statistics.stdev(samples) if len(samples) > 1 else 0.0,
    }


def format_ms(label, s):
    print(f"{label:<48} {s['median']:8.2f} ms  "
          f"(p10 {s['p10']:7.2f}, p90 {s['p90']:7.2f}, n={s['n']})")


def format_overhead(label, value, base, suffix=""):
    pct = 100.0 * value / base if base else 0.0
    print(f"{label:<48} {value:+8.2f} ms  ({pct:+6.1f}%{suffix})")


def print_section(title):
    print("")
    print(title)
    print("-" * len(title))


def bench_interleaved(run_one, labels, reps, warmup):
    """Run cases in shuffled order per repetition to reduce drift."""
    for _ in range(warmup):
        for label in labels:
            run_one(label)
    results = {label: [] for label in labels}
    rng = random.Random(0xC0FFEE)
    for _ in range(reps):
        order = list(labels)
        rng.shuffle(order)
        for label in order:
            start = time.perf_counter()
            run_one(label)
            results[label].append((time.perf_counter() - start) * 1000)
    return results


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
#define VEC_CAT_IMPL_(A, B) A##B
#define VEC_CAT_(A, B) VEC_CAT_IMPL_(A, B)
#define VEC_CAT3_IMPL_(A, B, C) A##B##C
#define VEC_CAT3_(A, B, C) VEC_CAT3_IMPL_(A, B, C)
#define Vec_push(T) VEC_CAT3_(Vec_, T, _push)
typedef struct { T value; } VEC_CAT_(Vec_, T);
static inline void Vec_push(T)(VEC_CAT_(Vec_, T) *v, int x) {
  v->value.x = x;
}
""")
    write(os.path.join(root, "tmpl", "stc_vec.h"), """\
// -- template stc_vec(T)
#include "../stc/stc.h"
#define STC_VEC_CAT_IMPL_(A, B) A##B
#define STC_VEC_CAT_(A, B) STC_VEC_CAT_IMPL_(A, B)
#define stc_vec(T) STC_VEC_CAT_(stc_vec_, T)
#ifndef STC_BENCH_CSTR
#define STC_BENCH_CSTR
typedef char cstr;
#endif
typedef struct { i_key value; } i_type;
""")
    os.makedirs(os.path.join(root, "stc"))
    write(os.path.join(root, "stc", "stc.h"), """\
#ifndef STC_BENCH_STC_H
#define STC_BENCH_STC_H
#endif
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
    vec_gen = os.path.join(root, "gen", "vec.h")
    stc_gen = os.path.join(root, "gen", "stc_vec.h")
    write(vec_gen, BASELINE_VEC_GEN)
    write(stc_gen, BASELINE_STC_GEN)

    def usages(n, offset):
        lines = []
        for i in range(n):
            t = (offset + i) % TYPES
            lines.append(f"  Vec(Type{t}) v{i};")
            lines.append(f"  Vec_push(Type{t})(&v{i}, {i});")
        return "\n".join(lines)

    main = f"""\
{include_block}
#include "gen/vec.h"
#include "gen/stc_vec.h"

int main(void) {{
{usages(USAGES_PER_MAIN, 0)}
  stc_vec(cstr) s = {{'a'}};
  return s.value == 'a' ? 0 : 1;
}}
"""
    write(os.path.join(root, "main.c"), main)

    sources = [os.path.join(root, "main.c")]
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
        sources.append(path)

    # A usage that exists only after macro expansion. It exercises the opt-in
    # preprocess mode without changing the default raw-text workload.
    macro_source = os.path.join(root, "src", "macro.c")
    write(macro_source, """\
#include "../type0.h"
#include "../gen/vec.h"

#define MAKE_VEC(T) Vec(T)

int macro_use(void) {
  MAKE_VEC(Type0) hidden = {1};
  return hidden.value.x;
}
""")
    sources.append(macro_source)
    return sources, (vec_gen, stc_gen)


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


def plugin_log_text(root, tag):
    text = ""
    for path in glob.glob(os.path.join(root, "clangd-" + tag + "-*.log")):
        try:
            with open(path, errors="replace") as f:
                text += f.read()
        except OSError:
            pass
    return text


def plugin_scan_times(root, tag):
    return [float(m) for m in re.findall(
        r"\[c-autotmpl\] scanning .* in ([0-9.]+) ms",
        plugin_log_text(root, tag),
    )]


def plugin_prepare_samples(root, tag):
    """Returns (cold, warm, phases) from timing-enabled plugin logs."""
    pattern = re.compile(
        r"\[c-autotmpl\] prepareUpdate in ([0-9.]+) ms "
        r"\(scan ([0-9.]+), usages ([0-9.]+), prune ([0-9.]+), "
        r"generate ([0-9.]+)\)"
    )
    cold, warm = [], []
    phases = {"scan": [], "usages": [], "prune": [], "generate": []}
    # One clangd session per log file: the first prepareUpdate is the cold
    # open, subsequent entries in that log are warm edits.
    for path in sorted(glob.glob(os.path.join(root,
                                              "clangd-" + tag + "-*.log"))):
        try:
            with open(path, errors="replace") as f:
                matches = pattern.findall(f.read())
        except OSError:
            continue
        if not matches:
            continue
        cold.append(float(matches[0][0]))
        warm.extend(float(m[0]) for m in matches[1:])
        for m in matches:
            for key, value in zip(phases, m[1:]):
                phases[key].append(float(value))
    return cold, warm, phases


def verify_plugin_load(root, tag, expect_loaded):
    loaded = "[c-autotmpl]" in plugin_log_text(root, tag)
    if expect_loaded and not loaded:
        raise RuntimeError("clangd did not load the feature module for " + tag)
    if not expect_loaded and loaded:
        raise RuntimeError("plugin loaded in the no-plugin baseline for "
                           + tag)


def lsp_parse_samples(root, main_file, plugin, clangd, tag, cold_reps,
                      warm_reps, timeout):
    cmd = [clangd, "--log=error"]
    if plugin:
        cmd.append("--load=" + plugin)
    uri = "file://" + main_file
    diagnostic_needle = '"uri":"' + uri + '"'
    env = bench_env()
    if plugin:
        env["C_AUTOTMPL_TIMING"] = "1"
    samples = []

    for cold in range(cold_reps):
        stdout_path = os.path.join(root, "clangd-" + tag + "-"
                                   + str(cold) + ".stdout")
        log_path = os.path.join(root, "clangd-" + tag + "-"
                                + str(cold) + ".log")
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
                wait_for_message(stdout_path, diagnostic_needle, before,
                                 timeout)
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
                    wait_for_message(stdout_path, diagnostic_needle, before,
                                     timeout)
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


def split_samples(samples, kind):
    return [t for k, t in samples if k == kind]


def parse_args(argv):
    args = {
        "files": DEFAULT_FILES,
        "headers": DEFAULT_HEADERS,
        "reps": DEFAULT_REPS,
        "cold_reps": DEFAULT_COLD_REPS,
        "warm_reps": DEFAULT_WARM_REPS,
        "warmup": DEFAULT_WARMUP,
        "timeout": DEFAULT_TIMEOUT,
        "no_lsp": False,
    }
    positional = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--quick":
            args.update(reps=5, cold_reps=3, warm_reps=3, warmup=1)
        elif arg == "--no-lsp":
            args["no_lsp"] = True
        elif arg in ("--files", "--headers", "--reps", "--cold-reps",
                     "--warm-reps", "--warmup", "--timeout"):
            if i + 1 >= len(argv):
                raise SystemExit("missing value for " + arg)
            key = arg[2:].replace("-", "_")
            args[key] = int(argv[i + 1])
            i += 1
        elif arg.startswith("--"):
            raise SystemExit("unknown option: " + arg)
        else:
            positional.append(arg)
        i += 1
    while len(positional) < 2:
        positional.append(None)
    return positional[0], positional[1], args


def main():
    cli_arg, plugin_arg, args = parse_args(sys.argv[1:])
    cli = os.path.abspath(cli_arg or "build/autotmpl")
    plugin = os.path.abspath(plugin_arg or "build/libc-autotmpl.so")

    clang = os.environ.get("CLANG") or shutil.which("clang") or \
        "/home/elias/.local/build/llvm-main-plugin/bin/clang"
    clangd = os.environ.get("CLANGD") or shutil.which("clangd") or "clangd"
    for path in (cli, plugin, clang):
        if not os.path.exists(path):
            print("missing executable: " + path, file=sys.stderr)
            return 1
    if not args["no_lsp"] and not shutil.which(clangd) \
            and not os.path.exists(clangd):
        print("missing executable: " + clangd, file=sys.stderr)
        return 1

    preprocess_cc = os.environ.get("CC") or "cc"
    root = tempfile.mkdtemp(prefix="c-autotmpl-bench-")
    try:
        sources, gen_paths = make_project(root, args["files"],
                                          args["headers"])
        baselines = {path: open(path).read() for path in gen_paths}
        write_conf(root, "profile=stc\nstc_auto=1\ndispatch=1\n",
                   preprocess_cc)
        setup = run([cli, "--root", root] + sources, root, capture=True,
                    env=bench_env())
        require_ok(setup, [cli])
        generated = {path: open(path).read() for path in gen_paths}
        compile_cmd = [clang, "-fsyntax-only", "-std=c23"] + sources
        check = run(compile_cmd, root, capture=True, env=bench_env())
        require_ok(check, [clang, "-fsyntax-only"])

        print(f"project: {len(sources)} sources, "
              f"{args['headers'] + TYPES + 4} headers, "
              f"{TYPES} guarded types, "
              f"{USAGES_PER_MAIN * 2} Vec usages in main.c, "
              "1 macro-only usage")
        print(f"samples: CLI/compile {args['reps']}, cold "
              f"{args['cold_reps']}, clangd warm {args['warm_reps']}, "
              f"warmup {args['warmup']}")

        print_section("Steady-state compile")
        def compile_case(label):
            if label == "clang":
                cmd = compile_cmd
            else:
                write_conf(root, "", preprocess_cc)
                dry = [cli, "--root", root, "--dry-run"] + sources
                require_ok(run(dry, root, env=bench_env()), dry)
                cmd = compile_cmd
            proc = run(cmd, root, env=bench_env())
            require_ok(proc, cmd)
        compile_results = bench_interleaved(
            compile_case, ("clang", "clang+autotmpl"), args["reps"],
            args["warmup"],
        )
        format_ms("clang -fsyntax-only", stats(compile_results["clang"]))
        format_ms("clang + autotmpl --dry-run",
                  stats(compile_results["clang+autotmpl"]))
        format_overhead("  added pre-build time",
                        stats(compile_results["clang+autotmpl"])["median"]
                        - stats(compile_results["clang"])["median"],
                        stats(compile_results["clang"])["median"],
                        " of compile")

        print_section("CLI dry-run by configuration")
        conf_by_label = dict(CLI_SCENARIOS)
        def cli_case(label):
            write_conf(root, conf_by_label[label], preprocess_cc)
            cmd = [cli, "--root", root, "--dry-run"] + sources
            proc = run(cmd, root, env=bench_env())
            require_ok(proc, cmd)
        labels = tuple(label for label, _ in CLI_SCENARIOS)
        cli_results = bench_interleaved(cli_case, labels, args["reps"],
                                        args["warmup"])
        raw_median = stats(cli_results["raw"])["median"]
        for label in labels:
            s = stats(cli_results[label])
            format_ms("autotmpl --dry-run " + label, s)
            if label != "raw":
                format_overhead("  vs raw scan", s["median"] - raw_median,
                                raw_median)

        print_section("Cold generation by configuration")
        cold_conf = dict(COLD_SCENARIOS)
        def cold_case(label):
            missing = gen_paths if label.startswith("preprocess") else ()
            write_state(root, baselines, missing)
            write_conf(root, cold_conf[label], preprocess_cc)
            cmd = [cli, "--root", root] + sources
            proc = run(cmd, root, env=bench_env())
            require_ok(proc, cmd)
        cold_labels = tuple(label for label, _ in COLD_SCENARIOS)
        cold_results = bench_interleaved(cold_case, cold_labels,
                                         args["cold_reps"], args["warmup"])
        cold_raw = stats(cold_results["raw"])["median"]
        for label in cold_labels:
            s = stats(cold_results[label])
            format_ms("autotmpl cold generation " + label, s)
            if label != "raw":
                format_overhead("  vs cold raw", s["median"] - cold_raw,
                                cold_raw)
        write_state(root, generated)
        write_conf(root, "", preprocess_cc)

        if not args["no_lsp"]:
            print_section("clangd parse and plugin configuration")
            base_samples = lsp_parse_samples(
                root, sources[0], None, clangd, "base", args["cold_reps"],
                args["warm_reps"], args["timeout"],
            )
            verify_plugin_load(root, "base", expect_loaded=False)
            plugin_samples = {}
            for tag, label, conf in PLUGIN_SCENARIOS:
                write_state(root, generated)
                write_conf(root, conf, preprocess_cc)
                samples = lsp_parse_samples(
                    root, sources[0], plugin, clangd, tag, args["cold_reps"],
                    args["warm_reps"], args["timeout"],
                )
                verify_plugin_load(root, tag, expect_loaded=True)
                plugin_samples[tag] = samples

            for kind in ("cold", "warm"):
                base = stats(split_samples(base_samples, kind))
                format_ms(f"clangd {kind} parse, no plugin", base)
                for tag, label, _ in PLUGIN_SCENARIOS:
                    s = stats(split_samples(plugin_samples[tag], kind))
                    format_ms(f"clangd {kind} parse, plugin {label}", s)
                    format_overhead("  plugin overhead",
                                    s["median"] - base["median"],
                                    base["median"])

            for tag, label, _ in PLUGIN_SCENARIOS:
                scan = plugin_scan_times(root, tag)
                if scan:
                    format_ms(f"registry scan, plugin {label}", stats(scan))
                prepare_cold, prepare_warm, phases = \
                    plugin_prepare_samples(root, tag)
                if prepare_cold:
                    format_ms(f"prepareUpdate first open, plugin {label}",
                              stats(prepare_cold))
                if prepare_warm:
                    format_ms(f"prepareUpdate edits, plugin {label}",
                              stats(prepare_warm))
                if tag == PLUGIN_SCENARIOS[0][0]:
                    for name in ("scan", "usages", "prune", "generate"):
                        if phases[name]:
                            format_ms("  " + name, stats(phases[name]))
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
