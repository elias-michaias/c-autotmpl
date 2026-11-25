#!/usr/bin/env python3
"""End-to-end clangd plugin smoke test.

Builds a tiny project and opens a source file with unsaved text in a real clangd
LSP session. The plugin must:

  * generate instantiations from that unsaved text before the first parse;
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
    "C_AUTOTMPL_DISPATCH_SUFFIX",
    "C_AUTOTMPL_DISPATCH_TEMPLATES",
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
        source = os.path.join(root, "main.c")
        write(source, "int main(void) { return 0; }\n")
        text = """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  $vec_push(&v, 7);
  $vec_set(7, v);
  return v.value == 7 ? 0 : 1;
}
"""
        generated = os.path.join(root, "gen", "vec.h")
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
                    "/* c-autotmpl dispatch begin vec */" in c and
                    "#ifdef C_AUTOTMPL_vec_int" in c and
                    "#define $vec_push(...)" in c and
                    "#define $vec_set(...)" in c
                ))
                if content is None:
                    print("plugin dispatch was not generated", file=sys.stderr)
                    with open(log_path, errors="replace") as f:
                        print(f.read()[-4000:], file=sys.stderr)
                    return 1
                if "/* c-autotmpl dispatch begin vec_push */" in content:
                    print("plugin scanned a dispatch macro as a type",
                          file=sys.stderr)
                    return 1

                send(proc, {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/definition",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 4, "character": 3},
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
                    "dispatch begin" not in c and
                    "/* c-autotmpl: vec(int) */" not in c
                ))
                if content is None:
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
