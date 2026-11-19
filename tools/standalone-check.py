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
#include "gen/box.h"
#include "gen/thing.h"

int main(void) {
  Thing(float) f = {12};
  Thing(Foo) u = {{34}};
  Thing(Pragma) p = {{56}};
  Thing(Box(float)) n = {{{78}}};
  Thing(Box(Thing(float))) *deep = 0;
  return f.value + u.value.x + p.value.y + n.value.value + (deep != 0);
}
""")
        write(os.path.join(root, "raw.c"), """\
int raw(void) {
  return Thing_double;
}
""")

        proc = subprocess.run(
            [cli, "--root", root, "--verbose",
             os.path.join(root, "main.c"), os.path.join(root, "raw.c")],
            cwd=root, capture_output=True, text=True,
        )
        output = proc.stdout + proc.stderr
        require(proc.returncode == 0, "autotmpl exited with error", output)

        for needle in ("generated Thing(float)", "generated Thing(Foo)",
                       "generated Thing(double)", "generated Box(float)",
                       "generated Thing(Box_float)",
                       "generated Box(Thing_float)",
                       "generated Thing(Box_Thing_float)",
                       "skipping Thing(Pragma): no traditional include guard"):
            require(needle in output, "missing output: " + needle, output)

        with open(gen_path) as f:
            content = f.read()
        for needle in ("/* c-autotmpl: Thing(float) */",
                       "/* c-autotmpl: Thing(Foo) */",
                       "#ifdef FOO_H",
                       "#define T Foo",
                       "/* c-autotmpl: Thing(double) */",
                       "/* c-autotmpl: Thing(Box_float) */",
                       '#ifndef C_AUTOTMPL_BOX_FLOAT_',
                       '#ifdef GEN_BOX_H',
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
                       "#ifdef GEN_THING_H"):
            require(needle in box_content,
                    "missing generated box text: " + needle, box_content)

        # A second run must take the cheap existing-instantiation path.
        noop = subprocess.run(
            [cli, "--root", root, "--dry-run", "--verbose",
             os.path.join(root, "main.c"), os.path.join(root, "raw.c")],
            cwd=root, capture_output=True, text=True,
        )
        noop_output = noop.stdout + noop.stderr
        require(noop.returncode == 0, "no-op run exited with error",
                noop_output)
        require("would generate" not in noop_output,
                "existing instantiations were regenerated", noop_output)
        require("added=0" in noop_output,
                "no-op run did not report zero additions", noop_output)

        # Dry-run reports changes without touching the file.
        write(gen_path, baseline)
        dry = subprocess.run(
            [cli, "--root", root, "--dry-run", "--verbose",
             os.path.join(root, "main.c"), os.path.join(root, "raw.c")],
            cwd=root, capture_output=True, text=True,
        )
        dry_output = dry.stdout + dry.stderr
        require(dry.returncode == 0, "dry-run exited with error", dry_output)
        require("would generate Thing(float)" in dry_output,
                "dry-run did not report changes", dry_output)
        with open(gen_path) as f:
            require(f.read() == baseline, "dry-run modified generated header")

        print("standalone-check OK")
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
