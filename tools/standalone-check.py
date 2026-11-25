#!/usr/bin/env python3
"""Integration check for the standalone autotmpl executable."""

import os
import shutil
import subprocess
import sys
import tempfile


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


def env_with(**overrides):
    env = dict(os.environ)
    for key in CONFIG_KEYS:
        env.pop(key, None)
    env.update(overrides)
    return env


def require(condition, message, output=""):
    if not condition:
        print(message, file=sys.stderr)
        if output:
            print(output, file=sys.stderr)
        raise SystemExit(1)


def check_stc_profile(cli, mode):
    root = tempfile.mkdtemp(prefix="c-autotmpl-stc-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "stc_vec.h"), """\
// -- template stc_vec(T)
#define STC_VEC_CAT_I(A, B) A##B
#define STC_VEC_CAT(A, B) STC_VEC_CAT_I(A, B)
#define stc_vec(T) STC_VEC_CAT(stc_vec_, T)
#ifndef STC_TEST_CSTR
#define STC_TEST_CSTR
typedef char cstr;
#endif
typedef struct { i_key value; } i_type;
""")
        write(os.path.join(root, "tmpl", "stc_hashmap.h"), """\
// -- template stc_hashmap(Key, Val)
#define STC_HASHMAP_JOIN_I(A, B) A##B
#define STC_HASHMAP_JOIN(A, B) STC_HASHMAP_JOIN_I(A, B)
#define STC_HASHMAP_TYPE_I(A, B, C) A##B##C
#define STC_HASHMAP_TYPE(A, B, C) STC_HASHMAP_TYPE_I(A, B, C)
#define stc_hashmap(Key, Val) \\
  STC_HASHMAP_TYPE(stc_hashmap_, Key, STC_HASHMAP_JOIN(_, Val))
#ifndef STC_TEST_CSTR
#define STC_TEST_CSTR
typedef char cstr;
#endif
typedef struct { i_key key; i_val value; } i_type;
""")
        write(os.path.join(root, "foo.h"), """\
#ifndef FOO_H
#define FOO_H
typedef struct { int x; } Foo;
#endif
""")
        write(os.path.join(root, "main.c"), """\
#include "foo.h"
#include "gen/stc_hashmap.h"
#include "gen/stc_vec.h"

static inline void stc_vec_Foo_push(stc_vec(Foo) *v, int x) {
  v->value.x = x;
}

int main(void) {
  stc_vec(Foo) v = {{0}};
  stc_vec_push(Foo)(&v, 7);
  stc_vec(cstr) s = {'a'};
  stc_hashmap(Foo, cstr) m = {{9}, 'b'};
  return v.value.x == 7 && s.value == 'a' && m.key.x == 9 &&
                 m.value == 'b' ? 0 : 1;
}
""")
        args = [cli, "--root", root, "--verbose"]
        env = env_with()
        if mode == "env":
            env["C_AUTOTMPL_PROFILE"] = "stc"
        elif mode == "conf":
            write(os.path.join(root, "autotmpl.conf"),
                  "# project settings\nprofile = stc\n")
        elif mode == "conf-over-env":
            write(os.path.join(root, "autotmpl.conf"), "profile=stc\n")
            env["C_AUTOTMPL_PROFILE"] = "none"
        elif mode == "flag-over-conf":
            write(os.path.join(root, "autotmpl.conf"), "profile=none\n")
            args += ["--profile", "stc"]
        elif mode == "profile":
            args += ["--profile", "stc"]
        else:
            args += ["--stc"]
        args.append(os.path.join(root, "main.c"))
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "STC profile run exited with error",
                output)
        require("generated stc_vec(Foo)" in output,
                "derived STC usage was not generated", output)
        require("generated stc_hashmap(Foo, cstr)" in output,
                "derived STC value usage was not generated", output)
        require("no traditional include guard" not in output,
                "derived STC type was guard-checked incorrectly", output)

        rerun = subprocess.run(args, cwd=root, capture_output=True, text=True,
                               env=env)
        require(rerun.returncode == 0 and "added=0" in rerun.stdout + rerun.stderr,
                "STC profile output was not idempotent", rerun.stdout + rerun.stderr)

        with open(os.path.join(root, "gen", "stc_vec.h")) as f:
            content = f.read()
        for needle in ("/* c-autotmpl: stc_vec(Foo) */",
                       "#ifndef C_AUTOTMPL_stc_vec_Foo",
                       "#ifdef FOO_H",
                       "#define i_type stc_vec_Foo",
                       "#define i_key Foo",
                       "#define stc_vec_push(...) "
                       "C_AUTOTMPL_STC_ADAPTER_CAT(stc_vec(__VA_ARGS__), _push)",
                       "#undef i_key\n#undef i_type",
                       "/* c-autotmpl: stc_vec(cstr) */",
                       "#define i_type stc_vec_cstr",
                       "#define i_key cstr",
                       "#define i_traits (c_pro_key | c_use_eq)",
                       "#undef i_traits\n#undef i_key\n#undef i_type"):
            require(needle in content, "missing STC preset text: " + needle,
                    content)
        require("#define T Foo" not in content,
                "STC preset leaked the legacy parameter macro", content)
        require(content.count("#define i_traits") == 1,
                "STC preset added traits to a plain type", content)
        require(content.count("/* c-autotmpl: stc_vec(Foo) */") == 1,
                "STC profile duplicated an existing block", content)

        with open(os.path.join(root, "gen", "stc_hashmap.h")) as f:
            map_content = f.read()
        for needle in ("/* c-autotmpl: stc_hashmap(Foo, cstr) */",
                       "#define i_type stc_hashmap_Foo_cstr",
                       "#define i_key Foo",
                       "#define i_val cstr",
                       "#define i_traits (c_pro_val)",
                       "#undef i_traits\n#undef i_val\n#undef i_key\n#undef i_type"):
            require(needle in map_content,
                    "missing STC map preset text: " + needle, map_content)

        compiler = os.environ.get("CC") or next(
            (name for name in ("cc", "gcc", "clang") if shutil.which(name)),
            None,
        )
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "stc-demo"],
                cwd=root, capture_output=True, text=True,
            )
            build_output = built.stdout + built.stderr
            require(built.returncode == 0,
                    "STC profile output did not compile", build_output)
            ran = subprocess.run([os.path.join(root, "stc-demo")], cwd=root)
            require(ran.returncode == 0, "STC profile output did not run")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def check_none_over_config(cli):
    root = tempfile.mkdtemp(prefix="c-autotmpl-none-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
typedef struct { T value; } vec(T);
""")
        write(os.path.join(root, "autotmpl.conf"), """\
profile=stc
stc_templates=vec
dispatch=1
""")
        write(os.path.join(root, "main.c"), """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  return v.value == 0 ? 0 : 1;
}
""")
        env = env_with(C_AUTOTMPL_PROFILE="stc",
                       C_AUTOTMPL_STC_TEMPLATES="vec",
                       C_AUTOTMPL_DISPATCH="1")
        args = [cli, "--root", root, "--verbose", "--profile", "none",
                os.path.join(root, "main.c")]
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "--profile none run exited with error",
                output)
        with open(os.path.join(root, "gen", "vec.h")) as f:
            content = f.read()
        require("#define T int" in content,
                "plain template block was not generated", content)
        require("#define i_type vec_int" not in content,
                "--profile none did not override config STC opt-ins", content)
        require("#define vec_push(...) " not in content,
                "--profile none generated an STC adapter", content)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def check_unprefixed_stc(cli):
    root = tempfile.mkdtemp(prefix="c-autotmpl-unprefixed-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
typedef struct { i_key value; } i_type;
static inline void vec_int_push(vec(int) *v, int x) { v->value = x; }
static inline void vec_int_frobnicate(vec(int) *v) { v->value += 1; }
""")
        write(os.path.join(root, "main.c"), """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  vec_push(int)(&v, 7);
  vec_frobnicate(int)(&v);
  return v.value == 8 ? 0 : 1;
}
""")
        env = env_with(C_AUTOTMPL_PROFILE="stc",
                       C_AUTOTMPL_STC_TEMPLATES="vec")
        args = [cli, "--root", root, "--verbose", os.path.join(root, "main.c")]
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "unprefixed STC run exited with error",
                output)
        require("generated vec(int)" in output,
                "unprefixed STC usage was not generated", output)
        require("stc templates=1" in output,
                "unprefixed STC template list was not reported", output)
        require("skipping vec(push)" not in output,
                "adapter call was scanned as a mangled type", output)

        with open(os.path.join(root, "gen", "vec.h")) as f:
            content = f.read()
        require(content.count("/* c-autotmpl: vec(int) */") == 1,
                "initial unprefixed block was duplicated", content)
        require("/* c-autotmpl: vec(push) */" not in content,
                "spurious adapter usage was generated", content)
        for needle in ("/* c-autotmpl: vec(int) */",
                       "#ifndef C_AUTOTMPL_vec_int",
                       "#define i_type vec_int",
                       "#define i_key int",
                       "#define vec_push(...) "
                       "C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _push)",
                       "#define vec_frobnicate(...) "
                       "C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), "
                       "_frobnicate)",
                       "#undef i_key\n#undef i_type"):
            require(needle in content,
                    "missing unprefixed STC text: " + needle, content)

        compiler = os.environ.get("CC") or next(
            (name for name in ("cc", "gcc", "clang") if shutil.which(name)),
            None,
        )
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "unprefixed-demo"],
                cwd=root, capture_output=True, text=True,
            )
            require(built.returncode == 0,
                    "generated STC adapter did not compile",
                    built.stdout + built.stderr)
            ran = subprocess.run([os.path.join(root, "unprefixed-demo")],
                                 cwd=root)
            require(ran.returncode == 0, "generated STC adapter did not run")

        # A new operation in existing source updates the generated header
        # without rewriting the existing instantiation block.
        with open(os.path.join(root, "tmpl", "vec.h"), "a") as f:
            f.write("static inline void vec_int_clear(vec(int) *v) "
                    "{ v->value = 0; }\n")
        write(os.path.join(root, "main.c"), """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  vec_clear(int)(&v);
  return v.value == 0 ? 0 : 1;
}
""")
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0,
                "new adapter operation did not update the header", output)
        with open(os.path.join(root, "gen", "vec.h")) as f:
            content = f.read()
        require("#define vec_clear(...) "
                "C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _clear)" in content,
                "new adapter macro was not appended", content)
        require(content.count("/* c-autotmpl: vec(int) */") == 1,
                "new adapter duplicated the instantiation block", content)

        # The CLI name list must work without environment configuration.
        os.remove(os.path.join(root, "gen", "vec.h"))
        args = [cli, "--root", root, "--verbose", "--profile", "stc",
                "--stc-template", "vec", os.path.join(root, "main.c")]
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env_with())
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0 and "generated vec(int)" in output,
                "CLI STC template list did not generate an unprefixed wrapper",
                output)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def check_stc_auto(cli):
    root = tempfile.mkdtemp(prefix="c-autotmpl-auto-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define vec(T) vec_##T
typedef struct { i_key value; } i_type;
static inline void vec_int_push(vec(int) *v, int x) { v->value = x; }
""")
        write(os.path.join(root, "main.c"), """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  vec_push(int)(&v, 7);
  return v.value == 7 ? 0 : 1;
}
""")
        env = env_with(C_AUTOTMPL_STC_AUTO="1")
        args = [cli, "--root", root, "--verbose", os.path.join(root, "main.c")]
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "STC auto run exited with error", output)
        require("stc auto=1" in output,
                "STC auto mode was not reported", output)
        require("generated vec(int)" in output,
                "STC auto did not detect the unprefixed wrapper", output)

        with open(os.path.join(root, "gen", "vec.h")) as f:
            content = f.read()
        require("/* c-autotmpl: vec(int) */" in content and
                "#define vec_push(...) " in content,
                "STC auto did not generate type and adapter", content)

        compiler = os.environ.get("CC") or next(
            (name for name in ("cc", "gcc", "clang") if shutil.which(name)),
            None,
        )
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "auto-demo"],
                cwd=root, capture_output=True, text=True,
            )
            require(built.returncode == 0,
                    "STC auto output did not compile",
                    built.stdout + built.stderr)
            ran = subprocess.run([os.path.join(root, "auto-demo")], cwd=root)
            require(ran.returncode == 0, "STC auto output did not run")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def check_dispatch(cli):
    root = tempfile.mkdtemp(prefix="c-autotmpl-dispatch-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
        write(os.path.join(root, "tmpl", "vec.h"), """\
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
#define VEC_FN_II(A, B) A##B
#define VEC_FN_I(A, B) VEC_FN_II(A, B)
#define VEC_FN(A, B) VEC_FN_I(A, B)
typedef struct { i_key value; } i_type;
static inline void VEC_FN(i_type, _push)(i_type *v, i_key x) { v->value = x; }
static inline void VEC_FN(i_type, _clear)(i_type *v) { v->value = 0; }
static inline int VEC_FN(i_type, _size)(i_type *v) { (void)v; return 1; }
static inline int VEC_FN(i_type, _pick)(int x, i_type v) { return x + (int)v.value; }
#ifndef VEC_HELPER_DEFINED
#define VEC_HELPER_DEFINED
static inline int helper(int x) { return x; }
#endif
""")
        write(os.path.join(root, "tmpl", "pair.h"), """\
// -- template type pair(A, B)
#define PAIR_CAT4_I(A, B, C, D) A##B##C##D
#define PAIR_CAT4(A, B, C, D) PAIR_CAT4_I(A, B, C, D)
#define pair(A, B) PAIR_CAT4(pair_, A, _, B)
#define PAIR_FN_5(A, B, C, D, E) A##B##C##D##E
#define PAIR_FN_2(A, B, C) PAIR_FN_5(pair_, A, _, B, C)
#define PAIR_FN(A, B, C) PAIR_FN_2(A, B, C)
#ifndef A
typedef int A;
#endif
#ifndef B
typedef float B;
#endif
typedef struct { A first; B second; } pair(A, B);
static inline A PAIR_FN(A, B, _sum)(int extra, pair(A, B) p) {
  return (A)(p.first + p.second + extra);
}
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
        write(source, """\
#include "gen/vec.h"
#include "gen/pair.h"
#include "gen/box.h"

int main(void) {
  vec(int) v = {0};
  vec(float) f = {0};
  pair(int, float) p = {1, 2.5f};
  Box(int) b = {7};
  $vec_push(&v, 7);
  $vec_push(&f, 2);
  $vec_clear(&v);
  $vec_pick(7, v);
  $Box_free(&b);
  return ($vec_size(&v) == 1 && v.value == 0 && $vec_pick(7, v) == 7 &&
          $vec_size(&f) == 1 && $vec_pick(3, f) == 5 &&
          (int)$pair_sum(3, p) == 6 && b.value == 0) ? 0 : 1;
}
""")
        generated = os.path.join(root, "gen", "vec.h")
        generated_pair = os.path.join(root, "gen", "pair.h")
        generated_box = os.path.join(root, "gen", "box.h")
        write(os.path.join(root, "autotmpl.conf"), """\
# project settings
profile=stc
stc_templates=vec
dispatch=1
""")
        env = env_with(C_AUTOTMPL_PROFILE="none",
                       C_AUTOTMPL_STC_TEMPLATES="nope",
                       C_AUTOTMPL_DISPATCH="0")
        args = [cli, "--root", root, "--verbose", source]
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "dispatch run exited with error", output)
        for needle in ("dispatch=1", "dispatch backend=generic",
                       "dispatch style=prefix", "dispatch affix=$",
                       "generated vec(int)", "generated vec(float)",
                       "generated pair(int, float)", "generated Box(int)"):
            require(needle in output, "missing dispatch report: " + needle,
                    output)

        with open(generated) as f:
            content = f.read()
        for needle in ("/* c-autotmpl dispatch begin vec */",
                       "/* c-autotmpl dispatch end vec */",
                       "#ifdef C_AUTOTMPL_vec_int",
                       "vec_int: vec_int_push",
                       "vec_int: vec_int_clear",
                       "vec_int: vec_int_size",
                       "vec_int: vec_int_pick",
                       "#define $vec_push(...)",
                       "#define $vec_clear(...)",
                       "#define $vec_size(...)",
                       "#define $vec_pick(...)"):
            require(needle in content, "missing dispatch text: " + needle,
                    content)
        require("helper" not in content,
                "ordinary helper was treated as a dispatch operation",
                content)
        with open(generated_pair) as f:
            pair_content = f.read()
        for needle in ("/* c-autotmpl dispatch begin pair */",
                       "#ifdef C_AUTOTMPL_pair_int_2C_20float",
                       "_Generic((p2), pair_int_float: pair_int_float_sum",
                       "#define $pair_sum(...)"):
            require(needle in pair_content,
                    "two-arity dispatch text is missing: " + needle,
                    pair_content)
        require("c-autotmpl dispatch begin vec_push" not in content,
                "dispatch macro call was scanned as a mangled type", content)
        with open(generated_box) as f:
            box_content = f.read()
        for needle in ("/* c-autotmpl dispatch begin Box */",
                       "Box_int: box_alloc_int", "Box_int*: box_alloc_int",
                       "Box_int: box_free_int", "Box_int*: box_free_int",
                       "#define $Box_alloc(...)", "#define $Box_free(...)"):
            require(needle in box_content,
                    "lowercase dispatch text is missing: " + needle,
                    box_content)
        require("box_secret" not in box_content and
                "$Box_secret(...)" not in box_content,
                "implementation-only helper was treated as a dispatch op",
                box_content)

        compiler = os.environ.get("CC") or next(
            (name for name in ("cc", "gcc", "clang") if shutil.which(name)),
            None,
        )
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "dispatch-demo"],
                cwd=root, capture_output=True, text=True,
            )
            build_output = built.stdout + built.stderr
            require(built.returncode == 0, "dispatch output did not compile",
                    build_output)
            ran = subprocess.run([os.path.join(root, "dispatch-demo")],
                                 cwd=root)
            require(ran.returncode == 0, "dispatch output did not run")

        rerun = subprocess.run(args, cwd=root, capture_output=True, text=True,
                               env=env)
        output = rerun.stdout + rerun.stderr
        require(rerun.returncode == 0 and "added=0" in output,
                "dispatch output was not idempotent", output)
        with open(generated) as f:
            content = f.read()
        require(content.count("/* c-autotmpl dispatch begin vec */") == 1,
                "dispatch section was duplicated", content)

        os.remove(generated)
        os.remove(generated_pair)
        if os.path.exists(generated_box):
            os.remove(generated_box)
        builtin_env = dict(env)
        builtin_env["C_AUTOTMPL_DISPATCH_BACKEND"] = "builtin"
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=builtin_env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0 and "dispatch backend=builtin" in output,
                "builtin dispatch backend was not selected", output)
        with open(generated) as f:
            content = f.read()
        for needle in ("__builtin_types_compatible_p(__typeof__(p1), vec_int)",
                       "__builtin_types_compatible_p(__typeof__(p1), vec_int*)",
                       "__builtin_choose_expr"):
            require(needle in content,
                    "builtin dispatch selectors are missing: " + needle,
                    content)
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "builtin-demo"],
                cwd=root, capture_output=True, text=True,
            )
            require(built.returncode == 0,
                    "builtin dispatch output did not compile",
                    built.stdout + built.stderr)

        os.remove(generated)
        os.remove(generated_pair)
        if os.path.exists(generated_box):
            os.remove(generated_box)
        write(source, """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  vec_push_dispatch(&v, 7);
  return v.value == 7 ? 0 : 1;
}
""")
        suffix_env = dict(env)
        suffix_env["C_AUTOTMPL_DISPATCH_SUFFIX"] = "_dispatch"
        suffix_env["C_AUTOTMPL_DISPATCH_TEMPLATES"] = "box"
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=suffix_env)
        require(proc.returncode == 0, "filtered dispatch run exited with error",
                proc.stdout + proc.stderr)
        with open(generated) as f:
            content = f.read()
        require("dispatch begin" not in content,
                "template filter did not disable dispatch", content)

        suffix_env["C_AUTOTMPL_DISPATCH_TEMPLATES"] = "vec"
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=suffix_env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0 and "dispatch templates=1" in output,
                "custom dispatch suffix run exited with error", output)
        with open(generated) as f:
            content = f.read()
        require("#define vec_push_dispatch(...)" in content and
                "#define vec_push$(...)" not in content,
                "custom dispatch suffix was not used", content)
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "suffix-demo"],
                cwd=root, capture_output=True, text=True,
            )
            require(built.returncode == 0,
                    "custom suffix dispatch output did not compile",
                    built.stdout + built.stderr)

        os.remove(generated)
        if os.path.exists(generated_pair):
            os.remove(generated_pair)
        write(source, """\
#include "gen/vec.h"

int main(void) {
  vec(int) v = {0};
  mvec_push(&v, 7);
  return v.value == 7 ? 0 : 1;
}
""")
        prefix_env = dict(env)
        prefix_env["C_AUTOTMPL_DISPATCH_AFFIX"] = "m"
        prefix_env["C_AUTOTMPL_DISPATCH_STYLE"] = "prefix"
        prefix_env["C_AUTOTMPL_DISPATCH_TEMPLATES"] = "vec"
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=prefix_env)
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0 and "dispatch affix=m" in output and
                "dispatch style=prefix" in output,
                "custom prefix dispatch run exited with error", output)
        with open(generated) as f:
            content = f.read()
        require("#define mvec_push(...)" in content and
                "#define $vec_push(...)" not in content,
                "custom dispatch affix was not used as a prefix", content)
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "main.c", "-o", "prefix-demo"],
                cwd=root, capture_output=True, text=True,
            )
            require(built.returncode == 0,
                    "custom prefix dispatch output did not compile",
                    built.stdout + built.stderr)

        os.remove(generated)
        if os.path.exists(generated_pair):
            os.remove(generated_pair)
        os.remove(os.path.join(root, "autotmpl.conf"))
        write(source, """\
#include "gen/vec.h"
#include "gen/pair.h"

int main(void) {
  vec(int) v = {0};
  pair(int, float) p = {1, 2.5f};
  return v.value == 0 && p.first == 1 ? 0 : 1;
}
""")
        proc = subprocess.run(args, cwd=root, capture_output=True, text=True,
                              env=env_with(C_AUTOTMPL_PROFILE="stc",
                                           C_AUTOTMPL_STC_TEMPLATES="vec"))
        require(proc.returncode == 0, "default dispatch run exited with error",
                proc.stdout + proc.stderr)
        with open(generated) as f:
            content = f.read()
        require("dispatch begin" not in content,
                "dispatch was enabled without opt-in", content)
    finally:
        shutil.rmtree(root, ignore_errors=True)

def main():
    cli = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else "build/autotmpl"
    )
    if not os.path.exists(cli):
        print("executable not found: " + cli, file=sys.stderr)
        return 1

    for mode in ("profile", "env", "stc", "conf", "conf-over-env",
                 "flag-over-conf"):
        check_stc_profile(cli, mode)
    check_none_over_config(cli)
    check_unprefixed_stc(cli)
    check_stc_auto(cli)
    check_dispatch(cli)

    root = tempfile.mkdtemp(prefix="c-autotmpl-standalone-")
    try:
        os.makedirs(os.path.join(root, "tmpl"))
        os.makedirs(os.path.join(root, "gen"))
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
        write(os.path.join(root, "tmpl", "apply.h"), """\
// -- template function apply(T)
#define APPLY_CAT_IMPL_(A, B) A##B
#define APPLY_CAT_(A, B) APPLY_CAT_IMPL_(A, B)
#define apply(T) APPLY_CAT_(apply_, T)
static inline T apply(T)(T value) { return value; }
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
        write(os.path.join(root, "tmpl", "wrap.h"), """\
// -- template type Wrap(F)
#ifndef F
#define F Option
#endif
#define WRAP_CAT_IMPL_(A, B) A##B
#define WRAP_CAT_(A, B) WRAP_CAT_IMPL_(A, B)
#define Wrap(F) WRAP_CAT_(Wrap_, F)
typedef struct { F(int) value; } Wrap(F);
#undef F
""")
        write(os.path.join(root, "tmpl", "split.h"), """\
// -- template Split(T)
#ifndef TMPL_HEADER
#define SPLIT_CAT_I(A, B) A##B
#define SPLIT_CAT(A, B) SPLIT_CAT_I(A, B)
#define Split(T) SPLIT_CAT(Split_, T)
#define SPLIT_GET(T) SPLIT_CAT(Split(T), _get)
typedef struct { T value; } Split(T);
T SPLIT_GET(T)(Split(T) *p) { return p->value; }
#else
#define SPLIT_CAT_I(A, B) A##B
#define SPLIT_CAT(A, B) SPLIT_CAT_I(A, B)
#define Split(T) SPLIT_CAT(Split_, T)
#define SPLIT_GET(T) SPLIT_CAT(Split(T), _get)
typedef struct { T value; } Split(T);
T SPLIT_GET(T)(Split(T) *p);
#endif
""")
        write(os.path.join(root, "pragma.h"), """\
#pragma once
typedef struct { int y; } Pragma;
""")
        baseline = """\
#ifndef GEN_THING_H
#define GEN_THING_H

#define T int
#include "../tmpl/thing.h"
#undef T

#endif
"""
        gen_path = os.path.join(root, "gen", "thing.h")
        write(gen_path, baseline)
        write(os.path.join(root, "main.c"), """\
#include "foo.h"
#include "pragma.h"
#include "gen/apply.h"
#include "gen/box.h"
#include "gen/select.h"
#include "gen/split.h"
#include "gen/sum.h"
#include "gen/thing.h"
#include "gen/user.h"
#include "gen/wrap.h"

int main(void) {
  Thing(float) f = {12};
  Thing(Foo) u = {{34}};
  Thing(Pragma) p = {{56}};
  Thing(Box(float)) n = {{{78}}};
  Thing(Box(Thing(float))) *deep = 0;
  Foo foo = {34};
  int a = select(int, Foo)(7, foo);
  int b = select(int, float)(9, 1.5f);
  int c = apply(int)(5);
  float d = apply(float)(2.5f);
  Box(int) box = {3};
  int e = select(Box(int), float)(box, 1.5f).value;
  Thing(int) sum_input = {1};
  int f = sum(int)(&sum_input);
  User(Option, int) user = { { 7, 1 } };
  int g = user.email.valid ? user.email.value : 1;
  Wrap(Option) wrap = { { 9, 1 } };
  int h = wrap.value.valid ? wrap.value.value : 1;
  Split(int) sp = {10};
  Split(Foo) sf = {{11}};
  return f.value + u.value.x + p.value.y + n.value.value +
         (deep != 0) + a + b + c + (int)d + e + f + g + h +
         sp.value + sf.value.x;
}
""")
        write(os.path.join(root, "raw.c"), """\
int raw(void) {
  return Thing_double + select_impl(1, 2) + apply_impl(3) +
         select(int)(1, 2) + select(int, float, char)(1, 2, 3) +
         apply(int, int)(1, 2);
}
""")
        sources = [os.path.join(root, name) for name in
                   ("main.c", "raw.c", "tmpl/apply.h", "tmpl/select.h")]

        proc = subprocess.run(
            [cli, "--root", root, "--verbose"] + sources,
            cwd=root, capture_output=True, text=True,
        )
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "autotmpl exited with error", output)

        for needle in ("generated Thing(float)", "generated Thing(Foo)",
                       "generated Thing(double)", "generated Box(float)",
                       "generated Thing(Box_float)",
                       "generated Box(Thing_float)",
                       "generated Thing(Box_Thing_float)",
                       "generated select(int, Foo)",
                       "generated select(int, float)",
                       "generated select(Box_int, float)",
                       "generated apply(int)",
                       "generated apply(float)",
                       "generated sum(int)",
                       "generated Option(int)",
                       "generated User(Option, int)",
                       "generated Wrap(Option)",
                       "generated Split(int)",
                       "generated Split(Foo)",
                       "generated impl Split(int)",
                       "generated impl Split(Foo)",
                       "generated makefile fragment",
                       "skipping Thing(Pragma): no traditional include guard"):
            require(needle in output, "missing output: " + needle, output)
        for needle in ("select(impl", "apply(impl", "select(First, Second)",
                       "apply(T)", "Thing(T)", "User(F, Payload)", "Wrap(F)",
                       "select(int)", "select(int, float, char)",
                       "apply(int, int)"):
            require(needle not in output,
                    "helper, placeholder, or wrong arity was scanned: " + needle,
                    output)

        with open(gen_path) as f:
            content = f.read()
        for needle in ("/* c-autotmpl: Thing(float) */",
                       "/* c-autotmpl: Thing(Foo) */",
                       "#ifdef FOO_H",
                       "#define T Foo",
                       "/* c-autotmpl: Thing(double) */",
                       "/* c-autotmpl: Thing(Box_float) */",
                       '#ifndef C_AUTOTMPL_Box_float',
                       "#define T Box_float",
                       "/* c-autotmpl: Box(Thing_float) */",
                       "/* c-autotmpl: Thing(Box_Thing_float) */"):
            require(needle in content, "missing generated text: " + needle,
                    content)
        require("Thing(Pragma)" not in content,
                "#pragma once type was generated", content)

        with open(os.path.join(root, "gen", "box.h")) as f:
            box_content = f.read()
        for needle in ("/* c-autotmpl: Box(float) */",
                       "/* c-autotmpl: Box(Thing_float) */",
                       "#ifndef C_AUTOTMPL_Box_Thing_float"):
            require(needle in box_content,
                    "missing generated box text: " + needle, box_content)

        with open(os.path.join(root, "gen", "select.h")) as f:
            select_content = f.read()
        for needle in ("/* c-autotmpl: select(int, Foo) */",
                       "/* c-autotmpl: select(int, float) */",
                       "/* c-autotmpl: select(Box_int, float) */",
                       "#ifndef C_AUTOTMPL_select_int_2C_20Foo",
                       "#ifndef C_AUTOTMPL_select_Box_int_2C_20float",
                       "#ifndef C_AUTOTMPL_Box_int",
                       "#ifdef FOO_H",
                       "#define First int",
                       "#define Second Foo",
                       "#define First Box_int",
                       "#define Second float",
                       "#undef Second",
                       "#undef First"):
            require(needle in select_content,
                    "missing generated select text: " + needle,
                    select_content)
        for needle in ("select(impl", "select(First, Second)"):
            require(needle not in select_content,
                    "helper or placeholder was emitted as a function instantiation",
                    select_content)

        with open(os.path.join(root, "gen", "apply.h")) as f:
            apply_content = f.read()
        for needle in ("/* c-autotmpl: apply(int) */",
                       "/* c-autotmpl: apply(float) */",
                       "#ifndef C_AUTOTMPL_apply_int",
                       "#define T int",
                       "#define T float",
                       "#undef T"):
            require(needle in apply_content,
                    "missing generated apply text: " + needle, apply_content)
        require("apply(T)" not in apply_content,
                "placeholder was emitted as a function instantiation",
                apply_content)

        with open(os.path.join(root, "gen", "sum.h")) as f:
            sum_content = f.read()
        for needle in ("/* c-autotmpl: Thing(int) */",
                       "/* c-autotmpl: sum(int) */",
                       "#ifndef C_AUTOTMPL_sum_int",
                       "#define Item int",
                       "#undef Item"):
            require(needle in sum_content,
                    "missing generated sum text: " + needle, sum_content)
        require("#ifdef GEN_THING_H" not in sum_content,
                "header dependency guard leaked into the flattened chain",
                sum_content)

        with open(os.path.join(root, "gen", "user.h")) as f:
            user_content = f.read()
        for needle in ("/* c-autotmpl: Option(int) */",
                       "/* c-autotmpl: User(Option, int) */",
                       "#ifndef C_AUTOTMPL_User_Option_2C_20int",
                       "#define F Option",
                       "#define Payload int",
                       "#undef Payload\n#undef F"):
            require(needle in user_content,
                    "missing generated HKT text: " + needle, user_content)
        require("#ifdef GEN_OPTION_H" not in user_content,
                "constructor dependency guard leaked into the flattened chain",
                user_content)

        with open(os.path.join(root, "gen", "wrap.h")) as f:
            wrap_content = f.read()
        for needle in ("/* c-autotmpl: Option(int) */",
                       "/* c-autotmpl: Wrap(Option) */",
                       "#ifndef C_AUTOTMPL_Wrap_Option",
                       "#define F Option",
                       "#undef F"):
            require(needle in wrap_content,
                    "missing generated single-argument HKT text: " + needle,
                    wrap_content)

        with open(os.path.join(root, "gen", "split.h")) as f:
            split_content = f.read()
        for needle in ("/* c-autotmpl: Split(int) */",
                       "/* c-autotmpl: Split(Foo) */",
                       "#define TMPL_HEADER 1",
                       "#undef TMPL_HEADER\n#undef T",
                       "#ifdef FOO_H"):
            require(needle in split_content,
                    "missing split public text: " + needle, split_content)

        split_impl = os.path.join(root, "gen", "impl", "split.c")
        with open(split_impl) as f:
            split_impl_content = f.read()
        for needle in ("#undef TMPL_HEADER",
                       '#include "../../foo.h"',
                       "/* c-autotmpl: Split(int) */",
                       "/* c-autotmpl: Split(Foo) */",
                       "#ifndef C_AUTOTMPL_IMPL_Split_int",
                       "#define T int",
                       "#define T Foo"):
            require(needle in split_impl_content,
                    "missing split implementation text: " + needle,
                    split_impl_content)
        require("#define TMPL_HEADER" not in split_impl_content,
                "implementation TU defined TMPL_HEADER", split_impl_content)
        require('#include "../split.h"' not in split_impl_content,
                "implementation TU included the public header",
                split_impl_content)

        with open(os.path.join(root, "gen", "impl", "c-autotmpl.mk")) as f:
            fragment = f.read()
        require("C_AUTOTMPL_IMPL_SRCS += gen/impl/split.c" in fragment,
                "makefile fragment omitted split implementation", fragment)

        write(os.path.join(root, "impl_main.c"), """\
#include "foo.h"
#include "gen/split.h"

int main(void) {
  Split(int) a = {10};
  Split(Foo) b = {{11}};
  return SPLIT_GET(int)(&a) + SPLIT_GET(Foo)(&b).x == 21 ? 0 : 1;
}
""")
        compiler = os.environ.get("CC") or next(
            (name for name in ("cc", "gcc", "clang") if shutil.which(name)),
            None,
        )
        if compiler:
            built = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "impl_main.c", "gen/impl/split.c", "-o", "split-demo"],
                cwd=root, capture_output=True, text=True,
            )
            build_output = built.stdout + built.stderr
            require(built.returncode == 0,
                    "generated implementation source did not compile",
                    build_output)
            ran = subprocess.run([os.path.join(root, "split-demo")], cwd=root)
            require(ran.returncode == 0, "generated implementation did not link")

        # A second run must take the cheap existing-instantiation path.
        noop = subprocess.run(
            [cli, "--root", root, "--dry-run", "--verbose"] + sources,
            cwd=root, capture_output=True, text=True,
        )
        noop_output = noop.stdout + noop.stderr
        require(noop.returncode == 0, "no-op run exited with error",
                noop_output)
        require("would generate" not in noop_output,
                "existing instantiations were regenerated", noop_output)
        require("added=0" in noop_output,
                "no-op run did not report zero additions", noop_output)

        # Dry-run reports changes without touching the files.
        write(gen_path, baseline)
        os.remove(split_impl)
        dry = subprocess.run(
            [cli, "--root", root, "--dry-run", "--verbose"] + sources,
            cwd=root, capture_output=True, text=True,
        )
        dry_output = dry.stdout + dry.stderr
        require(dry.returncode == 0, "dry-run exited with error", dry_output)
        require("would generate Thing(float)" in dry_output,
                "dry-run did not report header changes", dry_output)
        require("would generate impl Split(int)" in dry_output,
                "dry-run did not report implementation changes", dry_output)
        with open(gen_path) as f:
            require(f.read() == baseline, "dry-run modified generated header")
        require(not os.path.exists(split_impl),
                "dry-run created an implementation source")

        print("standalone-check OK")
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
