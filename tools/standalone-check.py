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


def require(condition, message, output=""):
    if not condition:
        print(message, file=sys.stderr)
        if output:
            print(output, file=sys.stderr)
        raise SystemExit(1)


def main():
    cli = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else "build/autotmpl"
    )
    if not os.path.exists(cli):
        print("executable not found: " + cli, file=sys.stderr)
        return 1

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
                       '#ifndef C_AUTOTMPL_BOX_FLOAT_',
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
                       "#ifndef C_AUTOTMPL_BOX_THING_FLOAT_"):
            require(needle in box_content,
                    "missing generated box text: " + needle, box_content)

        with open(os.path.join(root, "gen", "select.h")) as f:
            select_content = f.read()
        for needle in ("/* c-autotmpl: select(int, Foo) */",
                       "/* c-autotmpl: select(int, float) */",
                       "/* c-autotmpl: select(Box_int, float) */",
                       "#ifndef C_AUTOTMPL_SELECT_INT__FOO_",
                       "#ifndef C_AUTOTMPL_SELECT_BOX_INT__FLOAT_",
                       "#ifndef C_AUTOTMPL_BOX_INT_",
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
                       "#ifndef C_AUTOTMPL_APPLY_INT_",
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
                       "#ifndef C_AUTOTMPL_SUM_INT_",
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
                       "#ifndef C_AUTOTMPL_USER_OPTION__INT_",
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
                       "#ifndef C_AUTOTMPL_WRAP_OPTION_",
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
                       "#ifndef C_AUTOTMPL_IMPL_SPLIT_INT_",
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
