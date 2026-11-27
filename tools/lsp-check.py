#!/usr/bin/env python3
"""End-to-end clangd plugin smoke test.

Builds a tiny project and opens a source file with unsaved text in a real clangd
LSP session. The plugin must:

  * generate instantiations from that unsaved text before the first parse;
  * collect dispatch selectors into gen/dispatch.h with owner sections and one
    shared public macro per operation, including same-name methods from several
    templates;
  * expand nested applications such as Thing(Box(float)) and deep chains
    such as Thing(Box(Thing(float)));
  * guard user-defined types by their header guard;
  * skip #pragma once types;
  * treat existing hand-written blocks as already instantiated;
  * notice external edits to generated headers instead of restoring stale text;
  * prune blocks when a later didChange removes the usage.
"""

import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time


def send(proc, message):
    body = json.dumps(message).encode()
    proc.stdin.write(
        b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
    )
    proc.stdin.flush()


def receive_response(proc, wanted_id, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not select.select([proc.stdout], [], [], 0.2)[0]:
            continue
        headers = {}
        while True:
            line = proc.stdout.readline()
            if not line:
                return None
            if line in (b"\r\n", b"\n"):
                break
            key, _, value = line.decode("utf-8", "replace").partition(":")
            headers[key.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        body = b""
        while len(body) < length:
            chunk = proc.stdout.read(length - len(body))
            if not chunk:
                return None
            body += chunk
        message = json.loads(body)
        if message.get("id") == wanted_id:
            return message
    return None


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


CONFIG_KEYS = (
    "C_AUTOTMPL_PROFILE",
    "C_AUTOTMPL_STC_TEMPLATES",
    "C_AUTOTMPL_STC_AUTO",
    "C_AUTOTMPL_DISPATCH",
    "C_AUTOTMPL_DISPATCH_BACKEND",
    "C_AUTOTMPL_DISPATCH_STYLE",
    "C_AUTOTMPL_DISPATCH_AFFIX",
    "C_AUTOTMPL_DISPATCH_TEMPLATES",
    "C_AUTOTMPL_DISPATCH_TRUNCATE",
)


def plugin_env(**overrides):
    env = dict(os.environ)
    for key in CONFIG_KEYS:
        env.pop(key, None)
    env.update(overrides)
    return env


def wait_for_file(path, needles, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open(path, errors="replace") as f:
                content = f.read()
            if all(needle in content for needle in needles):
                return content
        except OSError:
            pass
        time.sleep(0.1)
    return None


def wait_for_content(path, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open(path, errors="replace") as f:
                content = f.read()
            if predicate(content):
                return content
        except OSError:
            pass
        time.sleep(0.1)
    return None


def messages(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return
    while True:
        sep = data.find(b"\r\n\r\n")
        if sep < 0:
            return
        header = data[:sep].decode("ascii", "replace")
        length = None
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
                break
        if length is None or len(data) < sep + 4 + length:
            return
        body = data[sep + 4:sep + 4 + length]
        data = data[sep + 4 + length:]
        try:
            yield json.loads(body)
        except json.JSONDecodeError:
            continue


def wait_for_diagnostics(stdout_path, uri, version, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for msg in messages(stdout_path):
            if msg.get("method") != "textDocument/publishDiagnostics":
                continue
            params = msg.get("params", {})
            if params.get("uri") != uri:
                continue
            if version is not None and params.get("version") != version:
                continue
            diags = params.get("diagnostics", [])
            if predicate(diags):
                return diags
        time.sleep(0.1)
    return None


def no_template_errors(diags):
    # Thing(Pragma) is intentionally unsupported, so its error is expected.
    for d in diags:
        text = d.get("message", "")
        if any(name in text for name in
               ("Thing_float", "Thing_Foo", "Thing_Bar", "Thing_Box",
                "Box_float", "Box_Thing_float", "Thing_Box_Thing_float",
                "select_int__Foo", "select_int__float", "sum_int",
               "Option_int", "User_Option", "Split_int", "vec_Foo",
               "vec_cstr")):
            return False
    return True


def check_dispatch(plugin):
    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-dispatch-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
typedef struct { i_key value; } i_type;
static inline void vec_int_push(i_type *v, int x) { v->value = x; }
static inline void vec_int_set(int x, i_type v) { v.value = x; }
""")
        write(os.path.join(root, "tmpl", "box.h"), """\
// -- template type Box(T)
#define BOX_CAT_I(A, B) A##B
#define BOX_CAT(A, B) BOX_CAT_I(A, B)
#define Box(T) BOX_CAT(Box_, T)
#define box_alloc(T) BOX_CAT(box_alloc_, T)
#define box_free(T) BOX_CAT(box_free_, T)
typedef struct { T value; } Box(T);
static inline Box(T) *box_alloc(T)(void) {
  static Box(T) value;
  return &value;
}
static inline void box_free(T)(Box(T) *b) { b->value = 0; }
#ifndef TMPL_HEADER
#define box_secret(T) BOX_CAT(box_secret_, T)
static inline int box_secret(T)(Box(T) *b) { return b->value; }
#endif
""")
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/vec.h"
#include "gen/box.h"
#include "gen/dispatch.h"

int main(void) {
  vec(int) v = {0};
  $vec_int_push(&v, 7);
  $vec_int_set(7, v);
  Box(int) *b = box_alloc(int)();
  $box_free(b);
  $bo
  return v.value == 7 && b->value == 0 ? 0 : 1;
}
"""
        generated = os.path.join(root, "gen", "vec.h")
        generated_box = os.path.join(root, "gen", "box.h")
        generated_dispatch = os.path.join(root, "gen", "dispatch.h")
        log_path = os.path.join(root, "clangd.log")
        uri = "file://" + source
        write(os.path.join(root, "autotmpl.conf"), """\
# project settings
profile=stc
stc_templates=vec
dispatch=1
""")
        env = plugin_env(C_AUTOTMPL_PROFILE="none",
                         C_AUTOTMPL_STC_TEMPLATES="nope",
                         C_AUTOTMPL_DISPATCH="0")
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=log, env=env,
            )
            try:
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "processId": None,
                        "rootUri": "file://" + root,
                        "capabilities": {
                            "textDocument": {
                                "completion": {
                                    "completionItem": {
                                        "snippetSupport": True,
                                    },
                                },
                            },
                        },
                    },
                })
                send(proc, {"jsonrpc": "2.0", "method": "initialized",
                            "params": {}})
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {
                        "uri": "file://" + source,
                        "languageId": "c",
                        "version": 1,
                        "text": text,
                    }},
                })
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: vec(int) */" in c and
                    "#ifdef C_AUTOTMPL_vec_int" in c
                ))
                if content is None:
                    print("plugin vec instantiation was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "c-autotmpl dispatch begin" in content:
                    print("plugin dispatch leaked into gen/vec.h",
                          file=sys.stderr)
                    return 1
                box_content = wait_for_content(generated_box, lambda c: (
                    "/* c-autotmpl: Box(int) */" in c and
                    "#ifdef C_AUTOTMPL_Box_int" in c
                ))
                if box_content is None:
                    print("plugin Box instantiation was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "c-autotmpl dispatch begin" in box_content:
                    print("plugin dispatch leaked into gen/box.h",
                          file=sys.stderr)
                    return 1
                dispatch_content = wait_for_content(
                    generated_dispatch,
                    lambda c: (
                        "/* c-autotmpl generated-by=plugin */" in c and
                        "/* c-autotmpl dispatch begin vec */" in c and
                        "/* c-autotmpl dispatch begin Box */" in c and
                        "#define $vec_int_push(...)" in c and
                        "#define $vec_int_set(...)" in c and
                        "#define $box_alloc(...)" in c and
                        "#define $box_free(...)" in c and
                        "Box_int: box_alloc_int" in c and
                        "Box_int: box_free_int" in c
                    ),
                )
                if dispatch_content is None:
                    print("plugin dispatch was not generated", file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if ("/* c-autotmpl dispatch begin vec_push */" in
                        dispatch_content):
                    print("plugin scanned a dispatch macro as a type",
                          file=sys.stderr)
                    return 1
                if "box_secret" in dispatch_content:
                    print("plugin exposed an implementation-only helper",
                          file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 6, "character": 3},
                    },
                })
                response = receive_response(proc, 3)
                locations = response.get("result") if response else None
                if (not locations or len(locations) != 1 or
                        not locations[0]["uri"].endswith("tmpl/vec.h") or
                        locations[0]["range"]["start"]["line"] != 5):
                    print("plugin did not map dispatch macro to template "
                          "function", file=sys.stderr)
                    print(response, file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                with open(os.path.join(root, "tmpl", "box.h")) as f:
                    box_header = f.read()
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 9, "character": 3},
                    },
                })
                response = receive_response(proc, 4)
                locations = response.get("result") if response else None
                if not locations or len(locations) != 1 or not locations[0][
                        "uri"].endswith("tmpl/box.h"):
                    print("plugin did not map lowercase dispatch macro",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                start = locations[0]["range"]["start"]
                start_offset = sum(len(line) + 1 for line in
                                   box_header.splitlines()[:start["line"]])
                start_offset += start["character"]
                if not box_header[start_offset:].startswith("box_free"):
                    print("dispatch pointed at the wrong declaration",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 5,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 10, "character": 2},
                    },
                })
                response = receive_response(proc, 5)
                result = response.get("result") if response else None
                items = result.get("items", []) if isinstance(result, dict) \
                    else (result or [])
                dispatch_items = [item for item in items
                                  if item.get("filterText") == "$box_free"]
                if len(dispatch_items) != 1:
                    print("plugin did not offer dispatch completion",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    return 1
                item = dispatch_items[0]
                snippet = item.get("insertText", "")
                edit_text = item.get("textEdit", {}).get("newText", "")
                if (item.get("insertTextFormat") != 2 or
                        not snippet.startswith("\\$box_free(") or
                        edit_text != snippet):
                    print("dispatch completion did not escape the '$' prefix",
                          file=sys.stderr)
                    print(item, file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": "file://" + source,
                                         "version": 2},
                        "contentChanges": [{"text":
                                            "int main(void) { return 0; }\n"}],
                    },
                })
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: vec(int) */" not in c
                ))
                dispatch_after = wait_for_content(
                    generated_dispatch,
                    lambda c: "dispatch begin" not in c,
                )
                if content is None or dispatch_after is None:
                    print("plugin dispatch was not pruned", file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
            finally:
                try:
                    send(proc, {"jsonrpc": "2.0", "id": 2,
                                "method": "shutdown", "params": None})
                    send(proc, {"jsonrpc": "2.0", "method": "exit",
                                "params": None})
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def check_dispatch_method(plugin):
    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-method-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "result.h"), """\
// -- template type Result(T, U)
#define RC4_I(A, B, C, D) A##B##C##D
#define RC4(A, B, C, D) RC4_I(A, B, C, D)
#define Result(T, U) RC4(Result_, T, _, U)
typedef struct { T ok; U err; } Result(T, U);
datatype(Result, T, U, (Ok, T), (Err, U))

// -- template function and_then_impl(T, U)
#define AT3_I(A, B, C) A##B##C
#define AT3(A, B, C) AT3_I(A, B, C)
#define and_then_impl(T, U) AT3(and_then_, T, U)
static inline Result(T, U) and_then_impl(T, U)(Result(T, U) r,
                                               Result(T, U) (*f)(T)) {
  return f(r.ok);
}

// -- template function map_impl(T, U, V)
// -- dispatch method Result.map explicit V
#define MC6_I(A, B, C, D, E, F) A##B##C##D##E##F
#define MC6(A, B, C, D, E, F) MC6_I(A, B, C, D, E, F)
#define map_impl(T, U, V) MC6(map_, T, _, U, _, V)
static inline Result(T, V) map_impl(T, U, V)(Result(T, U) r, V (*f)(T)) {
  Result(T, V) out;
  out.ok = f(r.ok);
  out.err = 0;
  return out;
}
""")
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/result.h"
#include "gen/dispatch.h"
static float from_int(int x) { return (float)x / 2.0f; }
static Result(int, float) keep(Result(int, float) r) { return r; }
int main(void) {
  Result(int, float) r = {7, 0};
  Result(int, float) chained = and_then(r, keep);
  Result(float, float) mapped = map(float)(chained, from_int);
  return (int)mapped.ok == 3 ? 0 : 1;
}
"""
        result_header = os.path.join(root, "gen", "result.h")
        dispatch_header = os.path.join(root, "gen", "dispatch.h")
        impl_header = os.path.join(root, "gen", "map_impl.h")
        log_path = os.path.join(root, "clangd.log")
        write(os.path.join(root, "autotmpl.conf"), """\
dispatch=1
dispatch_truncate=1
dispatch_templates=Result
""")
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=log, env=plugin_env(),
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
                send(proc, {"jsonrpc": "2.0", "method": "initialized",
                            "params": {}})
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {
                        "uri": "file://" + source,
                        "languageId": "c",
                        "version": 1,
                        "text": text,
                    }},
                })
                content = wait_for_content(result_header, lambda c: (
                    "/* c-autotmpl: Result(int, float) */" in c
                ))
                if content is None:
                    print("plugin Result instantiation was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "c-autotmpl dispatch begin" in content:
                    print("plugin dispatch leaked into gen/result.h",
                          file=sys.stderr)
                    return 1
                if "/* c-autotmpl: Result(float) */" in content:
                    print("plugin treated the explicit method arg as a type",
                          file=sys.stderr)
                    return 1
                dispatch = wait_for_content(dispatch_header, lambda c: (
                    "/* c-autotmpl generated-by=plugin */" in c and
                    "/* c-autotmpl dispatch begin Result.map */" in c and
                    "/* c-autotmpl dispatch begin shared.map */" in c and
                    "#define map(E1) C_AUTOTMPL_DISPATCH_CAT(map_, E1)" in c and
                    "Result_int_float: map_int_float_float" in c
                ))
                if dispatch is None:
                    print("plugin explicit-method dispatch was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if ("dispatch begin shared.Ok" in dispatch or
                        "dispatch begin shared.Err" in dispatch):
                    print("plugin generated datatype-owned constructor dispatch",
                          file=sys.stderr)
                    return 1
                impl = wait_for_content(impl_header, lambda c: (
                    "/* c-autotmpl: map_impl(int, float, float) */" in c
                ))
                if impl is None:
                    print("plugin explicit-method implementation was not "
                          "generated", file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                def position_of(needle):
                    idx = text.index(needle)
                    before = text[:idx]
                    return {
                        "line": before.count("\n"),
                        "character": len(before.rsplit("\n", 1)[-1]),
                    }

                with open(os.path.join(root, "tmpl", "result.h")) as f:
                    template_header = f.read()
                for request_id, needle, decl in (
                    (3, "map(float)", "map_impl"),
                    (4, "and_then(r", "and_then_impl"),
                ):
                    send(proc, {
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "method": "textDocument/definition",
                        "params": {
                            "textDocument": {"uri": "file://" + source},
                            "position": position_of(needle),
                        },
                    })
                    response = receive_response(proc, request_id)
                    locations = response.get("result") if response else None
                    if (not locations or len(locations) != 1 or
                            not locations[0]["uri"].endswith("tmpl/result.h")):
                        print("definition did not reach template function: " +
                              needle, file=sys.stderr)
                        print(response, file=sys.stderr)
                        with open(log_path, errors="replace") as f:
                            print(f.read()[-4000:], file=sys.stderr)
                        return 1
                    start = locations[0]["range"]["start"]
                    offset = sum(
                        len(line) + 1
                        for line in template_header.splitlines()[:start["line"]]
                    )
                    offset += start["character"]
                    if not template_header[offset:].startswith(decl):
                        print("definition pointed at the wrong declaration: " +
                              needle, file=sys.stderr)
                        print(response, file=sys.stderr)
                        return 1
            finally:
                try:
                    send(proc, {"jsonrpc": "2.0", "id": 2,
                                "method": "shutdown", "params": None})
                    send(proc, {"jsonrpc": "2.0", "method": "exit",
                                "params": None})
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def check_dispatch_truncate(plugin):
    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-truncate-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "box.h"), """\
// -- template type Box(T)
#define BOX_CAT_I(A, B) A##B
#define BOX_CAT(A, B) BOX_CAT_I(A, B)
#define Box(T) BOX_CAT(Box_, T)
#define box_set_impl(T) BOX_CAT(box_set_, T)
#define box_get_impl(T) BOX_CAT(box_get_, T)
typedef struct { T value; } Box(T);
static inline void box_set_impl(T)(Box(T) *b, T v) { b->value = v; }
static inline T box_get_impl(T)(Box(T) *b) { return b->value; }
""")
        write(os.path.join(root, "tmpl", "pair.h"), """\
// -- template type pair(T, U)
#define PAIR_CAT4_I(A, B, C, D) A##B##C##D
#define PAIR_CAT4(A, B, C, D) PAIR_CAT4_I(A, B, C, D)
#define PAIR_CAT5_I(A, B, C, D, E) A##B##C##D##E
#define PAIR_CAT5(A, B, C, D, E) PAIR_CAT5_I(A, B, C, D, E)
#define pair(T, U) PAIR_CAT4(pair_, T, _, U)
#define pair_set_impl(T, U) PAIR_CAT5(pair_set, _, T, _, U)
typedef struct { T first; U second; } pair(T, U);
static inline void pair_set_impl(T, U)(pair(T, U) *p, T a, U b) {
  p->first = a;
  p->second = b;
}
""")
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/box.h"
#include "gen/pair.h"
#include "gen/dispatch.h"

int main(void) {
  Box(int) a = {1};
  Box(float) b = {2};
  pair(int, float) p = {0};
  box_set(&a, 7);
  box_set(&b, 9);
  pair_set(&p, 3, 4.5f);
  pa
  return 0;
}
"""
        generated = os.path.join(root, "gen", "box.h")
        generated_pair = os.path.join(root, "gen", "pair.h")
        generated_dispatch = os.path.join(root, "gen", "dispatch.h")
        log_path = os.path.join(root, "clangd.log")
        uri = "file://" + source
        write(os.path.join(root, "autotmpl.conf"), """\
dispatch=1
dispatch_truncate=1
""")
        env = plugin_env(C_AUTOTMPL_DISPATCH="0",
                         C_AUTOTMPL_DISPATCH_AFFIX="m")
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=log, env=env,
            )
            try:
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "processId": None,
                        "rootUri": "file://" + root,
                        "capabilities": {
                            "textDocument": {
                                "completion": {
                                    "completionItem": {
                                        "snippetSupport": True,
                                    },
                                },
                            },
                        },
                    },
                })
                send(proc, {"jsonrpc": "2.0", "method": "initialized",
                            "params": {}})
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
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Box(int) */" in c and
                    "#ifdef C_AUTOTMPL_Box_int" in c
                ))
                if content is None:
                    print("plugin Box instantiation was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "c-autotmpl dispatch begin" in content:
                    print("plugin dispatch leaked into gen/box.h",
                          file=sys.stderr)
                    return 1

                pair_content = wait_for_content(generated_pair, lambda c: (
                    "/* c-autotmpl: pair(int, float) */" in c and
                    "#ifdef C_AUTOTMPL_pair_int_2C_20float" in c
                ))
                if pair_content is None:
                    print("plugin pair instantiation was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "c-autotmpl dispatch begin" in pair_content:
                    print("plugin dispatch leaked into gen/pair.h",
                          file=sys.stderr)
                    return 1

                dispatch = wait_for_content(generated_dispatch, lambda c: (
                    "/* c-autotmpl generated-by=plugin */" in c and
                    "/* c-autotmpl dispatch begin Box */" in c and
                    "/* c-autotmpl dispatch begin pair */" in c and
                    "/* c-autotmpl dispatch begin shared.box_set */" in c and
                    "/* c-autotmpl dispatch begin shared.pair_set */" in c and
                    "#define box_set(...)" in c and
                    "#define box_get(...)" in c and
                    "#define pair_set(...)" in c and
                    "Box_int: box_set_int" in c and
                    "Box_float: box_set_float" in c and
                    "pair_int_float: pair_set_int_float" in c
                ))
                if dispatch is None:
                    print("plugin truncated dispatch was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if ("dispatch begin box_set" in dispatch or
                        "C_AUTOTMPL_DISPATCH_SEL_Box_box_set_impl_" in dispatch):
                    print("plugin exposed an _impl name as a dispatch op",
                          file=sys.stderr)
                    return 1
                if ("#define $box_set(...)" in dispatch or
                        "#define mbox_set(...)" in dispatch):
                    print("plugin kept a dispatch affix in truncate mode",
                          file=sys.stderr)
                    return 1
                if "C_AUTOTMPL_DISPATCH_SEL_pair_pair_set_impl_" in dispatch:
                    print("plugin exposed a two-argument _impl op",
                          file=sys.stderr)
                    return 1

                with open(os.path.join(root, "tmpl", "box.h")) as f:
                    box_header = f.read()
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 8, "character": 3},
                    },
                })
                response = receive_response(proc, 3)
                locations = response.get("result") if response else None
                if (not locations or len(locations) != 1 or
                        not locations[0]["uri"].endswith("tmpl/box.h")):
                    print("plugin did not map bare dispatch call to op",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    return 1
                start = locations[0]["range"]["start"]
                start_offset = sum(len(line) + 1 for line in
                                   box_header.splitlines()[:start["line"]])
                start_offset += start["character"]
                if not box_header[start_offset:].startswith("box_set_impl"):
                    print("bare dispatch pointed at the wrong declaration",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 11, "character": 2},
                    },
                })
                response = receive_response(proc, 4)
                result = response.get("result") if response else None
                items = result.get("items", []) if isinstance(result, dict) \
                    else (result or [])
                dispatch_items = [item for item in items
                                  if item.get("filterText") == "pair_set"]
                if len(dispatch_items) != 1:
                    print("plugin did not offer bare dispatch completion",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    return 1
                item = dispatch_items[0]
                snippet = item.get("insertText", "")
                edit_text = item.get("textEdit", {}).get("newText", "")
                if (item.get("insertTextFormat") != 2 or
                        not snippet.startswith("pair_set(") or
                        "\\$" in snippet or edit_text != snippet):
                    print("bare dispatch completion had a bad snippet",
                          file=sys.stderr)
                    print(item, file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri,
                                         "version": 2},
                        "contentChanges": [{"text":
                                            "int main(void) { return 0; }\n"}],
                    },
                })
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Box(int) */" not in c
                ))
                dispatch_after = wait_for_content(
                    generated_dispatch,
                    lambda c: "dispatch begin" not in c,
                )
                if content is None or dispatch_after is None:
                    print("plugin truncated dispatch was not pruned",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
            finally:
                try:
                    send(proc, {"jsonrpc": "2.0", "id": 2,
                                "method": "shutdown", "params": None})
                    send(proc, {"jsonrpc": "2.0", "method": "exit",
                                "params": None})
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def check_shared_dispatch(plugin):
    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-shared-dispatch-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "option.h"), """\
// -- template type Option(T)
#define OCAT_I(A, B) A##B
#define OCAT(A, B) OCAT_I(A, B)
#define OCAT4_I(A, B, C, D) A##B##C##D
#define OCAT4(A, B, C, D) OCAT4_I(A, B, C, D)
#define Option(T) OCAT(Option_, T)
typedef struct { T value; } Option(T);

// -- template function option_map_impl(T, U)
// -- dispatch method Option.map explicit U
#define option_map_impl(T, U) OCAT4(map_, T, _, U)
static inline Option(U) option_map_impl(T, U)(Option(T) x, U (*f)(T)) {
  Option(U) out = {f(x.value)};
  return out;
}
""")
        write(os.path.join(root, "tmpl", "result.h"), """\
// -- template type Result(T, U)
#ifndef T
typedef int T;
#endif
#ifndef U
typedef float U;
#endif
#ifndef V
typedef float V;
#endif
#define RCAT4_I(A, B, C, D) A##B##C##D
#define RCAT4(A, B, C, D) RCAT4_I(A, B, C, D)
#define RCAT6_I(A, B, C, D, E, F) A##B##C##D##E##F
#define RCAT6(A, B, C, D, E, F) RCAT6_I(A, B, C, D, E, F)
#define Result(T, U) RCAT4(Result_, T, _, U)
typedef struct { T ok; U err; } Result(T, U);

// -- template function result_map_impl(T, U, V)
// -- dispatch method Result.map explicit V
#define result_map_impl(T, U, V) RCAT6(map_, T, _, U, _, V)
static inline Result(V, U) result_map_impl(T, U, V)(Result(T, U) x,
                                                    V (*f)(T)) {
  Result(V, U) out = {f(x.ok), x.err};
  return out;
}
""")
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/option.h"
#include "gen/result.h"
#include "gen/dispatch.h"

static int twice(int x) { return x * 2; }
static float half(int x) { return (float)x / 2.0f; }

int main(void) {
  Option(int) o = {7};
  Result(int, float) r = {11, 0};
  Option(int) om = map(int)(o, twice);
  Option(float) omf = map(float)(o, half);
  Result(int, float) rm = map(int)(r, twice);
  Result(float, float) rmf = map(float)(r, half);
  return (om.value == 14 && omf.value == 3.5f && rm.ok == 22 &&
          rmf.ok == 5.5f) ? 0 : 1;
}
"""
        option_header = os.path.join(root, "gen", "option.h")
        result_header = os.path.join(root, "gen", "result.h")
        dispatch_header = os.path.join(root, "gen", "dispatch.h")
        log_path = os.path.join(root, "clangd.log")
        cli_section = """\
/* c-autotmpl dispatch begin shared.cli_owned */
/* c-autotmpl generated-by=cli */
#define cli_owned_marker 1
/* c-autotmpl dispatch end shared.cli_owned */
"""
        write(dispatch_header, "#ifndef GEN_DISPATCH_H\n#define GEN_DISPATCH_H\n\n"
              + cli_section + "\n#endif\n")
        write(option_header, "#ifndef GEN_OPTION_H\n#define GEN_OPTION_H\n\n"
              + cli_section + "\n#endif\n")
        write(os.path.join(root, "autotmpl.conf"), """\
dispatch=1
dispatch_truncate=1
""")
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=log, env=plugin_env(),
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
                send(proc, {"jsonrpc": "2.0", "method": "initialized",
                            "params": {}})
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {
                        "uri": "file://" + source,
                        "languageId": "c",
                        "version": 1,
                        "text": text,
                    }},
                })
                for path, needle in (
                    (option_header, "/* c-autotmpl: Option(int) */"),
                    (result_header, "/* c-autotmpl: Result(int, float) */"),
                ):
                    content = wait_for_content(path, lambda c, n=needle: n in c)
                    if content is None:
                        print("plugin shared instantiation was not generated",
                              file=sys.stderr)
                        with open(log_path, errors="replace") as f:
                            print(f.read()[-4000:], file=sys.stderr)
                        return 1
                    leaked = any(
                        "generated-by=cli" not in section
                        for section in re.findall(
                            r"/\* c-autotmpl dispatch begin [^*]+ \*/"
                            r"(.*?)"
                            r"/\* c-autotmpl dispatch end [^*]+ \*/",
                            content, re.S,
                        )
                    )
                    if leaked:
                        print("plugin dispatch leaked into a per-type header",
                              file=sys.stderr)
                        return 1
                    generic = ("/* c-autotmpl: Result(V, U) */" in content or
                               "/* c-autotmpl: Option(U) */" in content)
                    if generic:
                        print("plugin generated a generic template body",
                              file=sys.stderr)
                        return 1

                dispatch = wait_for_content(
                    dispatch_header,
                    lambda c: (
                        "/* c-autotmpl generated-by=plugin */" in c and
                        "/* c-autotmpl dispatch begin Option.map */" in c and
                        "/* c-autotmpl dispatch begin Result.map */" in c and
                        "/* c-autotmpl dispatch begin shared.map */" in c and
                        "Option_int: map_int_int" in c and
                        "Result_int_float: map_int_float_int" in c and
                        "Result_int_float: map_int_float_float" in c and
                        c.count("#define map(E1) ") == 1 and
                        c.count("#define map_int(...) ") == 1 and
                        c.count("#define map_float(...) ") == 1
                    ),
                )
                if dispatch is None:
                    print("plugin shared dispatch was not generated",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                chained = re.search(
                    r"#define C_AUTOTMPL_DISPATCH_SEL_Result_map_int_1\(p1\) "
                    r"C_AUTOTMPL_DISPATCH_SEL_Option_map_int_1_\d+\(p1\)",
                    dispatch,
                ) or re.search(
                    r"#define C_AUTOTMPL_DISPATCH_SEL_Option_map_int_1\(p1\) "
                    r"C_AUTOTMPL_DISPATCH_SEL_Result_map_int_1_\d+\(p1\)",
                    dispatch,
                )
                if not chained:
                    print("plugin owner selectors were not chained",
                          file=sys.stderr)
                    return 1
                with open(dispatch_header) as f:
                    final_dispatch = f.read()
                with open(option_header) as f:
                    final_option = f.read()
                if ("#define cli_owned_marker 1" not in final_dispatch or
                        "#define cli_owned_marker 1" not in final_option):
                    print("plugin clobbered a CLI-owned dispatch section",
                          file=sys.stderr)
                    return 1

                needle = "map(float)(o"
                idx = text.index(needle)
                before = text[:idx]
                position = {
                    "line": before.count("\n"),
                    "character": len(before.rsplit("\n", 1)[-1]),
                }
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": "file://" + source},
                        "position": position,
                    },
                })
                response = receive_response(proc, 3)
                locations = response.get("result") if response else None
                owners = {loc["uri"] for loc in locations} \
                    if locations else set()
                if (not any(uri.endswith("tmpl/option.h") for uri in owners) or
                        not any(uri.endswith("tmpl/result.h")
                                for uri in owners)):
                    print("plugin did not map shared method to both owners",
                          file=sys.stderr)
                    print(response, file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                # Opening a template definition must not turn its generic
                # spellings (`Result(T, U)`) into concrete instantiations or
                # plugin-owned dispatch sections.
                with open(dispatch_header) as f:
                    before_dispatch = f.read()
                with open(result_header) as f:
                    before_result = f.read()
                template_result = os.path.join(root, "tmpl", "result.h")
                with open(template_result) as f:
                    template_text = f.read()
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {
                        "uri": "file://" + template_result,
                        "languageId": "c",
                        "version": 1,
                        "text": template_text,
                    }},
                })
                # A synchronous request forces clangd to finish processing the
                # didOpen notification before the generated files are compared.
                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 5,
                    "method": "textDocument/documentSymbol",
                    "params": {
                        "textDocument": {"uri": "file://" + template_result},
                    },
                })
                if receive_response(proc, 5) is None:
                    print("template header didOpen was not processed",
                          file=sys.stderr)
                    return 1
                with open(dispatch_header) as f:
                    after_dispatch = f.read()
                with open(result_header) as f:
                    after_result = f.read()
                if after_dispatch != before_dispatch or after_result != before_result:
                    print("template header didOpen wrote generated files",
                          file=sys.stderr)
                    print("dispatch changed:", after_dispatch != before_dispatch,
                          file=sys.stderr)
                    print("result changed:", after_result != before_result,
                          file=sys.stderr)
                    return 1
            finally:
                try:
                    send(proc, {"jsonrpc": "2.0", "id": 2,
                                "method": "shutdown", "params": None})
                    send(proc, {"jsonrpc": "2.0", "method": "exit",
                                "params": None})
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def check_value_generics(plugin):
    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-value-generic-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "array.h"), """\
// -- template type Array(T, N)
#define ARRAY_CAT4_I(a, b, c, d) a##b##c##d
#define ARRAY_CAT4(a, b, c, d) ARRAY_CAT4_I(a, b, c, d)
#define Array(T, N) ARRAY_CAT4(Array_, T, _, N)
typedef struct { T data[N]; } Array(T, N);
""")
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/array.h"

int main(void) {
  Array(int, 4) a = {{1, 2, 3, 4}};
  Array(float, 2) b = {{1.0f, 2.0f}};
  return a.data[3] == 4 && b.data[1] == 2.0f ? 0 : 1;
}
"""
        generated = os.path.join(root, "gen", "array.h")
        log_path = os.path.join(root, "clangd.log")
        uri = "file://" + source
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=log, env=plugin_env(),
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
                send(proc, {"jsonrpc": "2.0", "method": "initialized",
                            "params": {}})
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
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Array(int, 4) */" in c and
                    "/* c-autotmpl: Array(float, 2) */" in c and
                    "#define N 4" in c and
                    "#define N 2" in c
                ))
                if content is None:
                    print("plugin did not generate numeric value generics",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "#ifdef" in content:
                    print("plugin emitted a type guard for a numeric value",
                          file=sys.stderr)
                    print(content, file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri,
                                         "version": 2},
                        "contentChanges": [{"text":
                                            "int main(void) { return 0; }\n"}],
                    },
                })
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Array(int, 4) */" not in c and
                    "/* c-autotmpl: Array(float, 2) */" not in c
                ))
                if content is None:
                    print("plugin did not prune numeric value generics",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
            finally:
                try:
                    send(proc, {"jsonrpc": "2.0", "id": 2,
                                "method": "shutdown", "params": None})
                    send(proc, {"jsonrpc": "2.0", "method": "exit",
                                "params": None})
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def check_auto_config(plugin):
    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-auto-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
typedef struct { i_key value; } i_type;
static inline void vec_int_push(i_type *v, int x) { v->value = x; }
""")
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  vec_push(int)(&v, 7);
  return v.value == 7 ? 0 : 1;
}
"""
        generated = os.path.join(root, "gen", "vec.h")
        log_path = os.path.join(root, "clangd.log")
        stdout_path = os.path.join(root, "clangd.stdout")
        env = plugin_env(C_AUTOTMPL_STC_AUTO="1")
        with open(log_path, "wb") as log, open(stdout_path, "wb") as stdout:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root, stdin=subprocess.PIPE, stdout=stdout, stderr=log,
                env=env,
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
                send(proc, {"jsonrpc": "2.0", "method": "initialized",
                            "params": {}})
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {
                        "uri": "file://" + source,
                        "languageId": "c",
                        "version": 1,
                        "text": text,
                    }},
                })
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: vec(int) */" in c and
                    "#define vec_push(...) " in c and
                    "/* c-autotmpl: vec(push) */" not in c
                ))
                if content is None:
                    print("plugin C_AUTOTMPL_STC_AUTO did not generate",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
            finally:
                try:
                    send(proc, {"jsonrpc": "2.0", "id": 2,
                                "method": "shutdown", "params": None})
                    send(proc, {"jsonrpc": "2.0", "method": "exit",
                                "params": None})
                    proc.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    proc.kill()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def main():
    plugin = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else "build/libc-autotmpl.so"
    )
    if not os.path.exists(plugin):
        print("plugin not found: " + plugin, file=sys.stderr)
        return 1
    if check_auto_config(plugin) != 0:
        return 1
    if check_dispatch(plugin) != 0:
        return 1
    if check_dispatch_method(plugin) != 0:
        return 1
    if check_dispatch_truncate(plugin) != 0:
        return 1
    if check_shared_dispatch(plugin) != 0:
        return 1
    if check_value_generics(plugin) != 0:
        return 1

    root = tempfile.mkdtemp(prefix="c-autotmpl-lsp-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        # A nested .clangd must not hide the real root (it has no templates).
        os.makedirs(os.path.join(root, "nested"))
        write(os.path.join(root, "nested", ".clangd"),
              'CompileFlags:\n  Add: ["-std=c23"]\n')
        write(os.path.join(root, "tmpl", "thing.h"), """\
// -- template Thing(T)
#define Thing_IMPL(T) Thing_##T
#define Thing(T) Thing_IMPL(T)
#define CAT_IMPL_(A, B) A##B
#define CAT_(A, B) CAT_IMPL_(A, B)
#ifndef T
typedef int T;
#endif
typedef struct { T value; } CAT_(Thing_, T);
#undef T
""")
        write(os.path.join(root, "foo.h"), """\
#ifndef FOO_H
#define FOO_H
typedef struct { int x; } Foo;
#endif
""")
        write(os.path.join(root, "tmpl", "box.h"), """\
// -- template type Box(T)
#define Box_IMPL(T) Box_##T
#define Box(T) Box_IMPL(T)
#define BOX_CAT_IMPL_(A, B) A##B
#define BOX_CAT_(A, B) BOX_CAT_IMPL_(A, B)
#ifndef T
typedef int T;
#endif
typedef struct { T value; } BOX_CAT_(Box_, T);
#undef T
""")
        write(os.path.join(root, "tmpl", "select.h"), """\
// -- template function select(First, Second)
#define SELECT_CAT3_IMPL_(A, B, C) A##B##C
#define SELECT_CAT3_(A, B, C) SELECT_CAT3_IMPL_(A, B, C)
#define select(First, Second) SELECT_CAT3_(select_, First, Second)
static inline First select(First, Second)(First a, Second b) {
  (void)b;
  return a;
}
""")
        write(os.path.join(root, "tmpl", "sum.h"), """\
// -- template function sum(Item)
#define SUM_CAT_IMPL_(A, B) A##B
#define SUM_CAT_(A, B) SUM_CAT_IMPL_(A, B)
#define sum(Item) SUM_CAT_(sum_, Item)
static inline Item sum(Item)(Thing(Item) *xs) { return xs->value; }
""")
        write(os.path.join(root, "tmpl", "option.h"), """\
// -- template type Option(T)
#ifndef OPTION_CAT_DEFINED
#define OPTION_CAT_DEFINED
#define OPTION_CAT_IMPL_(A, B) A##B
#define OPTION_CAT_(A, B) OPTION_CAT_IMPL_(A, B)
#define Option(T) OPTION_CAT_(Option_, T)
#endif
typedef struct { T value; int valid; } Option(T);
#undef T
""")
        write(os.path.join(root, "tmpl", "user.h"), """\
// -- template type User(F, Payload)
#ifndef F
#define F Option
#endif
#ifndef Payload
typedef int Payload;
#endif
#define USER_CAT_IMPL_(A, B) A##B
#define USER_CAT_(A, B) USER_CAT_IMPL_(A, B)
#define User(F, Payload) USER_CAT_(User_, F)
typedef struct { F(Payload) email; } User(F, Payload);
#undef Payload
#undef F
""")
        write(os.path.join(root, "tmpl", "split.h"), """\
// -- template Split(T)
#ifndef TMPL_HEADER
#define SPLIT_CAT_I(A, B) A##B
#define SPLIT_CAT(A, B) SPLIT_CAT_I(A, B)
#define Split(T) SPLIT_CAT(Split_, T)
#define SPLIT_GET(T) SPLIT_CAT(Split_get_, T)
static T SPLIT_GET(T)(Split(T) *p) { return p->value; }
#else
#define SPLIT_CAT_I(A, B) A##B
#define SPLIT_CAT(A, B) SPLIT_CAT_I(A, B)
#define Split(T) SPLIT_CAT(Split_, T)
typedef struct { T value; } Split(T);
#endif
""")
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
#define VEC_FN_II(A, B) A##B
#define VEC_FN_I(A, B) VEC_FN_II(A, B)
#define VEC_FN(A, B) VEC_FN_I(A, B)
typedef char cstr;
typedef struct { i_key value; } i_type;
static inline int VEC_FN(i_type, _size)(i_type *v) { (void)v; return 7; }
static inline int VEC_FN(i_type, _frobnicate)(i_type *v) { (void)v; return 1; }
static inline int VEC_FN(i_type, _future)(i_type *v) { (void)v; return 1; }
""")
        write(os.path.join(root, "bar.h"), """\
#ifndef BAR_H
#define BAR_H
typedef struct { int z; } Bar;
#endif
""")
        write(os.path.join(root, "pragma.h"), """\
#pragma once
typedef struct { int y; } Pragma;
""")
        generated = os.path.join(root, "gen", "thing.h")
        select_generated = os.path.join(root, "gen", "select.h")
        sum_generated = os.path.join(root, "gen", "sum.h")
        user_generated = os.path.join(root, "gen", "user.h")
        split_generated = os.path.join(root, "gen", "split.h")
        stc_generated = os.path.join(root, "gen", "vec.h")
        split_impl = os.path.join(root, "gen", "impl", "split.c")
        impl_makefile = os.path.join(root, "gen", "impl", "c-autotmpl.mk")
        # A pre-existing hand-written block for Thing(float). The plugin must
        # treat it as already present rather than appending a second copy.
        manual_float = (
            '#define T float\n#include "../tmpl/thing.h"\n#undef T\n'
        )
        write(generated, """\
#ifndef GEN_THING_H
#define GEN_THING_H
#include "../tmpl/thing.h"

#define T float
#include "../tmpl/thing.h"
#undef T

#endif
""")
        source = os.path.join(root, "main.c")
        # Disk content deliberately has no usages: generation must use the
        # unsaved didOpen buffer.
        write(source, """\
#include "foo.h"
#include "pragma.h"
#include "gen/thing.h"

int main(void) { return 0; }
""")
        text1 = """\
#include "foo.h"
#include "pragma.h"
#include "gen/box.h"
#include "gen/select.h"
#include "gen/split.h"
#include "gen/vec.h"
#include "gen/sum.h"
#include "gen/thing.h"
#include "gen/user.h"

int main(void) {
  Thing(float) f = {12};
  Thing(Foo) u = {{34}};
  Thing(Pragma) p = {{56}};
  Thing(Box(float)) n = {{{78}}};
  Thing(Box(Thing(float))) *deep = 0;
  Foo foo = {34};
  int a = select(int, Foo)(7, foo);
  int b = select(int, float)(9, 1.5f);
  Thing(int) si = {7};
  int c = sum(int)(&si);
  User(Option, int) user = { { 7, 1 } };
  int d = user.email.valid ? user.email.value : 1;
  Split(int) sp = {9};
  int e = sp.value;
  vec(Foo) sv = {{11}};
  int g = sv.value.x;
  int g2 = vec_size(Foo)(&sv);
  int g3 = vec_frobnicate(Foo)(&sv);
  vec(cstr) sv2 = {'a'};
  int h = sv2.value;
  int h2 = vec_size(cstr)(&sv2);
  return f.value + u.value.x + p.value.y + n.value.value +
         (deep != 0) + a + b + c + d + e + g + g2 + g3 + h + h2;
}
"""
        text1_future = text1.replace(
            "vec_frobnicate(Foo)(&sv);",
            "vec_frobnicate(Foo)(&sv);\n  int g4 = vec_future(Foo)(&sv);",
        ).replace("+ g3 + h", "+ g3 + g4 + h")
        text2 = """\
#include "foo.h"
#include "pragma.h"
#include "gen/box.h"
#include "gen/thing.h"

int main(void) {
  Thing(float) f = {12};
  return f.value;
}
"""
        other_source = os.path.join(root, "other.c")
        write(other_source, """\
#include "bar.h"
#include "gen/thing.h"

int other(void) { return 0; }
""")
        other_text = """\
#include "bar.h"
#include "gen/thing.h"

int other(void) {
  Thing(Bar) b = {{78}};
  return b.value.z;
}
"""

        log_path = os.path.join(root, "clangd.log")
        stdout_path = os.path.join(root, "clangd.stdout")
        with open(log_path, "wb") as log, open(stdout_path, "wb") as stdout:
            proc = subprocess.Popen(
                ["clangd", "--log=verbose", "--load=" + plugin],
                cwd=root,
                stdin=subprocess.PIPE,
                stdout=stdout,
                stderr=log,
                env=plugin_env(C_AUTOTMPL_PROFILE="stc",
                               C_AUTOTMPL_STC_TEMPLATES="vec"),
            )
            try:
                uri = "file://" + source
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
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": uri,
                            "languageId": "c",
                            "version": 1,
                            "text": text1,
                        }
                    },
                })

                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Thing(Foo) */" in c and
                    "#ifdef FOO_H" in c and
                    "/* c-autotmpl: Thing(Box_float) */" in c and
                    '#ifndef C_AUTOTMPL_Box_float' in c and
                    '#define T Box_float' in c and
                    '/* c-autotmpl: Box(Thing_float) */' in c and
                    '/* c-autotmpl: Thing(Box_Thing_float) */' in c and
                    c.count('#define T float\n#include "../tmpl/thing.h"') == 1
                ))
                if content is None:
                    print("timed out waiting for unsaved-buffer generation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "/* c-autotmpl: Thing(float) */" in content:
                    print("plugin duplicated an existing hand-written block",
                          file=sys.stderr)
                    print(content, file=sys.stderr)
                    return 1

                select_content = wait_for_content(select_generated, lambda c: (
                    "/* c-autotmpl: select(int, Foo) */" in c and
                    "/* c-autotmpl: select(int, float) */" in c and
                    "#ifndef C_AUTOTMPL_select_int_2C_20Foo" in c and
                    "#ifdef FOO_H" in c and
                    "#define First int" in c and
                    "#define Second Foo" in c and
                    "#undef Second\n#undef First" in c
                ))
                if select_content is None:
                    print("timed out waiting for function-template generation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "select(impl" in select_content:
                    print("plugin treated a helper name as a function template",
                          file=sys.stderr)
                    print(select_content, file=sys.stderr)
                    return 1

                sum_content = wait_for_content(sum_generated, lambda c: (
                    "/* c-autotmpl: Thing(int) */" in c and
                    "/* c-autotmpl: sum(int) */" in c and
                    "#define Item int" in c and
                    "#ifdef GEN_THING_H" not in c
                ))
                if sum_content is None:
                    print("timed out waiting for header function dependencies",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                user_content = wait_for_content(user_generated, lambda c: (
                    "/* c-autotmpl: Option(int) */" in c and
                    "/* c-autotmpl: User(Option, int) */" in c and
                    "#define F Option" in c and
                    "#define Payload int" in c and
                    "#undef Payload\n#undef F" in c and
                    "#ifdef GEN_OPTION_H" not in c
                ))
                if user_content is None:
                    print("timed out waiting for lexical HKT generation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                split_content = wait_for_content(split_generated, lambda c: (
                    "/* c-autotmpl: Split(int) */" in c and
                    "#define TMPL_HEADER 1" in c and
                    "#undef TMPL_HEADER\n#undef T" in c
                ))
                if split_content is None:
                    print("timed out waiting for split-header generation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                stc_content = wait_for_content(stc_generated, lambda c: (
                    "/* c-autotmpl: vec(Foo) */" in c and
                    "#ifdef FOO_H" in c and
                    "#define i_type vec_Foo" in c and
                    "#define i_key Foo" in c and
                    "#define vec_size(...) "
                    "C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _size)" in c and
                    "#define vec_frobnicate(...) "
                    "C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), "
                    "_frobnicate)" in c and
                    "#undef i_key\n#undef i_type" in c and
                    "/* c-autotmpl: vec(cstr) */" in c and
                    "#define i_type vec_cstr" in c and
                    "#define i_key cstr" in c and
                    "#define i_traits (c_pro_key | c_use_eq)" in c and
                    "#undef i_traits\n#undef i_key\n#undef i_type" in c and
                    "/* c-autotmpl: vec(size) */" not in c and
                    "#define TMPL_HEADER" not in c and
                    "#define T Foo" not in c
                ))
                if stc_content is None:
                    print("timed out waiting for STC profile generation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 2},
                        "contentChanges": [{"text": text1_future}],
                    },
                })
                future_content = wait_for_content(stc_generated, lambda c: (
                    "#define vec_future(...) "
                    "C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _future)" in c and
                    c.count("/* c-autotmpl: vec(Foo) */") == 1
                ))
                if future_content is None:
                    print("plugin did not append a new adapter operation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                split_impl_content = wait_for_content(split_impl, lambda c: (
                    "/* c-autotmpl: Split(int) */" in c and
                    "/* Generated by c-autotmpl. Do not edit. */" in c and
                    "#undef TMPL_HEADER\n" in c and
                    '#include "../../tmpl/split.h"' in c and
                    "#ifndef C_AUTOTMPL_IMPL_Split_int" in c and
                    "#define TMPL_HEADER" not in c
                ))
                if split_impl_content is None:
                    print("timed out waiting for split-implementation generation",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                makefile_content = wait_for_content(impl_makefile, lambda c: (
                    "C_AUTOTMPL_IMPL_SRCS += gen/impl/split.c" in c
                ))
                if makefile_content is None:
                    print("timed out waiting for implementation makefile fragment",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                diags = wait_for_diagnostics(
                    stdout_path, uri, 1, no_template_errors, timeout=10)
                if diags is None:
                    print("clangd published template-name errors", file=sys.stderr)
                    with open(stdout_path, errors="replace") as f:
                        print(f.read()[-6000:], file=sys.stderr)
                    return 1

                published = []
                for msg in messages(stdout_path):
                    if msg.get("method") != "textDocument/publishDiagnostics":
                        continue
                    params = msg.get("params", {})
                    if params.get("uri") == uri and params.get("version") == 1:
                        published.append(params.get("diagnostics", []))
                if any(not no_template_errors(d) for d in published):
                    print("clangd briefly published template-name errors",
                          file=sys.stderr)
                    with open(stdout_path, errors="replace") as f:
                        print(f.read()[-6000:], file=sys.stderr)
                    return 1

                with open(log_path, errors="replace") as f:
                    early_log = f.read()
                if "[c-autotmpl] generated Thing(float)" in early_log:
                    print("plugin regenerated an existing hand-written block",
                          file=sys.stderr)
                    print(early_log[-4000:], file=sys.stderr)
                    return 1

                log_text = wait_for_file(
                    log_path,
                    ["skipping Thing(Pragma): no traditional include guard"],
                    timeout=10,
                )
                if log_text is None:
                    print("plugin did not reject the #pragma once type",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                if wait_for_file(log_path, ["workspace scan complete"],
                                 timeout=10) is None:
                    print("plugin did not finish its workspace scan",
                          file=sys.stderr)
                    return 1

                # Opening a generated implementation source must not feed its
                # re-include blocks back into the public-header generator.
                impl_source = split_impl
                with open(impl_source) as f:
                    impl_probe_text = f.read()
                impl_probe_text += """\

int impl_probe(void) {
  Thing(double) ignored = {1};
  return ignored.value;
}
"""
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": "file://" + impl_source,
                            "languageId": "c",
                            "version": 1,
                            "text": impl_probe_text,
                        }
                    },
                })
                time.sleep(1)
                with open(generated) as f:
                    thing_after_impl = f.read()
                if "Thing(double)" in thing_after_impl:
                    print("plugin generated from an implementation source",
                          file=sys.stderr)
                    return 1
                with open(log_path, errors="replace") as f:
                    if "[c-autotmpl] generated Thing(double)" in f.read():
                        print("plugin logged an implementation-source generation",
                              file=sys.stderr)
                        return 1

                # A second file introduces a new guarded type after the first
                # file's type index was already built.
                other_uri = "file://" + other_source
                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": other_uri,
                            "languageId": "c",
                            "version": 1,
                            "text": other_text,
                        }
                    },
                })
                content = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Thing(Bar) */" in c and
                    "#ifdef BAR_H" in c
                ))
                if content is None:
                    print("plugin missed a type from a later open file",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                diags = wait_for_diagnostics(
                    stdout_path, other_uri, 1, no_template_errors, timeout=10)
                if diags is None:
                    print("clangd published template-name errors in other.c",
                          file=sys.stderr)
                    return 1

                # Simulate the user cleaning up the old hand-written block.
                # The plugin has cached the generated file by now; a later
                # prune must not write that stale copy back.
                with open(generated) as f:
                    current = f.read()
                if manual_float not in current:
                    print("test setup lost the hand-written block",
                          file=sys.stderr)
                    return 1
                write(generated, current.replace(manual_float, "", 1))

                send(proc, {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 3},
                        "contentChanges": [{"text": text2}],
                    },
                })

                pruned = wait_for_content(generated, lambda c: (
                    "/* c-autotmpl: Thing(Foo) */" not in c and
                    "/* c-autotmpl: Thing(Box_float) */" not in c and
                    "/* c-autotmpl: Box(Thing_float) */" not in c and
                    "/* c-autotmpl: Thing(Box_Thing_float) */" not in c and
                    "/* c-autotmpl: Thing(float) */" in c and
                    c.count('#define T float\n#include "../tmpl/thing.h"') == 1
                ), timeout=10) is not None
                if not pruned:
                    print("plugin did not prune the unused Thing(Foo) block",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_select = wait_for_content(select_generated, lambda c: (
                    "/* c-autotmpl: select(int, Foo) */" not in c and
                    "/* c-autotmpl: select(int, float) */" not in c
                ), timeout=10) is not None
                if not pruned_select:
                    print("plugin did not prune unused function-template blocks",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_sum = wait_for_content(sum_generated, lambda c: (
                    "/* c-autotmpl: Thing(int) */" not in c and
                    "/* c-autotmpl: sum(int) */" not in c
                ), timeout=10) is not None
                if not pruned_sum:
                    print("plugin did not prune header-derived dependencies",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_user = wait_for_content(user_generated, lambda c: (
                    "/* c-autotmpl: Option(int) */" not in c and
                    "/* c-autotmpl: User(Option, int) */" not in c
                ), timeout=10) is not None
                if not pruned_user:
                    print("plugin did not prune lexical HKT blocks",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_split = wait_for_content(split_generated, lambda c: (
                    "/* c-autotmpl: Split(int) */" not in c
                ), timeout=10) is not None
                if not pruned_split:
                    print("plugin did not prune split-header blocks",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_stc = wait_for_content(stc_generated, lambda c: (
                    "/* c-autotmpl: vec(Foo) */" not in c and
                    "/* c-autotmpl: vec(cstr) */" not in c
                ), timeout=10) is not None
                if not pruned_stc:
                    print("plugin did not prune STC profile blocks",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_split_impl = wait_for_content(split_impl, lambda c: (
                    "/* c-autotmpl: Split(int) */" not in c
                ), timeout=10) is not None
                if not pruned_split_impl:
                    print("plugin did not prune split-implementation blocks",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                pruned_makefile = wait_for_content(impl_makefile, lambda c: (
                    "C_AUTOTMPL_IMPL_SRCS += gen/impl/split.c" not in c
                ), timeout=10) is not None
                if not pruned_makefile:
                    print("plugin did not update implementation makefile fragment",
                          file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1

                diags = wait_for_diagnostics(
                    stdout_path, uri, 2, no_template_errors, timeout=10)
                if diags is None:
                    print("clangd published errors after pruning",
                          file=sys.stderr)
                    return 1

                with open(log_path, errors="replace") as f:
                    log_text = f.read()
                for needle in ("[c-autotmpl] generated Thing(float)",
                               "[c-autotmpl] generated Thing(Foo)",
                               "[c-autotmpl] generated Thing(Bar)",
                               "[c-autotmpl] generated Box(float)",
                               "[c-autotmpl] generated Thing(Box_float)",
                               "[c-autotmpl] generated Box(Thing_float)",
                               "[c-autotmpl] generated Thing(Box_Thing_float)",
                               "[c-autotmpl] generated select(int, Foo)",
                               "[c-autotmpl] generated select(int, float)",
                               "[c-autotmpl] generated sum(int)",
                               "[c-autotmpl] generated Thing(int)",
                               "[c-autotmpl] generated Option(int)",
                               "[c-autotmpl] generated User(Option, int)",
                               "[c-autotmpl] generated Split(int)",
                                "[c-autotmpl] generated vec(Foo)",
                                "[c-autotmpl] generated vec(cstr)",
                               "[c-autotmpl] generated implementation Split(int)",
                               "removed /* c-autotmpl: Thing(Foo) */",
                               "removed /* c-autotmpl: Thing(Box_float) */",
                               "removed /* c-autotmpl: Box(Thing_float) */",
                               "removed /* c-autotmpl: Thing(Box_Thing_float) */",
                               "removed /* c-autotmpl: select(int, Foo) */",
                               "removed /* c-autotmpl: select(int, float) */",
                               "removed /* c-autotmpl: sum(int) */",
                               "removed /* c-autotmpl: Thing(int) */",
                               "removed /* c-autotmpl: Option(int) */",
                               "removed /* c-autotmpl: User(Option, int) */",
                               "removed /* c-autotmpl: Split(int) */"):
                    if needle not in log_text:
                        print("plugin did not log: " + needle, file=sys.stderr)
                        print(log_text[-4000:], file=sys.stderr)
                        return 1

                with open(generated) as f:
                    final_content = f.read()
                if "Thing(Pragma)" in final_content:
                    print("unguarded #pragma once type was instantiated",
                          file=sys.stderr)
                    return 1
                if "/* c-autotmpl: Thing(Bar) */" not in final_content:
                    print("later open file's instantiation was lost",
                          file=sys.stderr)
                    return 1
                with open(source) as f:
                    if "Thing(" in f.read():
                        print("test source was modified on disk",
                              file=sys.stderr)
                        return 1

                if os.environ.get("C_AUTOTMPL_TEST_VERBOSE"):
                    for line in log_text.splitlines():
                        if "[c-autotmpl]" in line:
                            print(line)
                print("lsp-check OK")
                return 0
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
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
