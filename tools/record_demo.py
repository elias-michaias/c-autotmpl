#!/usr/bin/env python3
"""Record a self-contained Neovim + tmux c-autotmpl demo as an animated GIF.

The fixture is embedded below, so the GIF only uses templates from this repo's
documented API. It walks through same-arity get() overloading with +owner, the
central dispatch header, shared map(V) overloading with Result, explicit method
type arguments, and a compile/run check.

Requires: tmux, nvim, Pillow, and the JetBrainsMonoNerdFontMono TTF files used
by tools/demo/init.lua (embedded below).
"""

import argparse
import shutil
import subprocess
import tempfile
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[1]
SESSION = "c-autotmpl-demo"

COLS = 78
FULL_ROWS = 49
SHELL_ROWS = 14
SPLIT_TOP_ROWS = FULL_ROWS - SHELL_ROWS - 1
CELL_W = 12
CELL_H = 21
FONT_SIZE = 19
W, H = 950, 1024
PAD_X = (W - COLS * CELL_W) // 2
CONTENT_H = (FULL_ROWS - 1) * CELL_H
PAD_Y = (H - CONTENT_H) // 2
SPLIT_COL = COLS // 2 - 1  # Neovim draws the split pipe in the left pane's last column

REGULAR = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf"
BOLD = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Bold.ttf"

DEFAULT_FG = (216, 222, 233)
DEFAULT_BG = (23, 26, 33)
WINDOW_BG = (15, 17, 22)
BORDER = (42, 48, 62)
SEPARATOR = (58, 66, 82)

font = ImageFont.truetype(REGULAR, FONT_SIZE)
font_bold = ImageFont.truetype(BOLD, FONT_SIZE)
fb = font.getbbox("M")
FONT_TOP = fb[1]
FONT_H = fb[3] - fb[1]

FIXTURE = Path()
AUTOTMPL = Path()
WINDOW_INDEX = "0"
PANE_INDEX = "0"
shell_visible = False


def pane_target(offset=0):
    return f"{SESSION}:{WINDOW_INDEX}.{int(PANE_INDEX) + offset}"


@dataclass
class Cell:
    ch: str = " "
    fg: tuple = DEFAULT_FG
    bg: tuple = DEFAULT_BG
    bold: bool = False
    underline: bool = False


XTERM_16 = [
    (0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0),
    (0, 0, 205), (205, 0, 205), (0, 205, 205), (229, 229, 229),
    (76, 76, 76), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]


def xterm256(n):
    if n < 16:
        return XTERM_16[n]
    if n < 232:
        n -= 16
        return ((n // 36) * 51, ((n % 36) // 6) * 51, (n % 6) * 51)
    v = 8 + (n - 232) * 10
    return (v, v, v)


def parse_capture(text, rows):
    grid = [[Cell() for _ in range(COLS)] for _ in range(rows)]
    state = {"fg": DEFAULT_FG, "bg": DEFAULT_BG, "bold": False,
             "reverse": False, "underline": False}

    def sgr(params):
        codes = [int(x) if x else 0 for x in params.split(";")]
        i = 0
        while i < len(codes):
            code = codes[i]
            if code == 0:
                state.update(fg=DEFAULT_FG, bg=DEFAULT_BG, bold=False,
                             reverse=False, underline=False)
            elif code == 1:
                state["bold"] = True
            elif code == 22:
                state["bold"] = False
            elif code == 4:
                state["underline"] = True
            elif code == 24:
                state["underline"] = False
            elif code == 7:
                state["reverse"] = True
            elif code == 27:
                state["reverse"] = False
            elif code == 39:
                state["fg"] = DEFAULT_FG
            elif code == 49:
                state["bg"] = DEFAULT_BG
            elif code == 38:
                if i + 1 < len(codes) and codes[i + 1] == 2:
                    state["fg"] = tuple(codes[i + 2:i + 5])
                    i += 4
                elif i + 1 < len(codes) and codes[i + 1] == 5:
                    state["fg"] = xterm256(codes[i + 2])
                    i += 2
            elif code == 48:
                if i + 1 < len(codes) and codes[i + 1] == 2:
                    state["bg"] = tuple(codes[i + 2:i + 5])
                    i += 4
                elif i + 1 < len(codes) and codes[i + 1] == 5:
                    state["bg"] = xterm256(codes[i + 2])
                    i += 2
            i += 1

    r = c = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\x1b":
            j = i + 1
            if j < len(text) and text[j] == "[":
                k = j + 1
                while k < len(text) and not ("@" <= text[k] <= "~"):
                    k += 1
                if k < len(text) and text[k] == "m":
                    sgr(text[j + 1:k])
                i = k + 1
            else:
                i += 2
            continue
        if ch == "\n":
            r += 1
            c = 0
            i += 1
            continue
        if ch == "\r":
            c = 0
            i += 1
            continue
        if r < rows and c < COLS:
            fg, bg = state["fg"], state["bg"]
            if state["reverse"]:
                fg, bg = bg, fg
            # Neovim renders the split pipe with the active-window fg, which
            # looks too bright and sits a few pixels off from the statusline
            # divider. Drop it and draw one aligned divider ourselves.
            if ch == "│":
                ch = " "
            grid[r][c] = Cell(ch, fg, bg, state["bold"], state["underline"])
        c += 1
        i += 1
    return grid


def tmux(*args):
    return subprocess.run(["tmux", *args], capture_output=True, text=True,
                          check=True).stdout


def capture_pane(pane, rows):
    out = tmux("capture-pane", "-e", "-p", "-t", pane_target(pane))
    lines = out.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    while len(lines) < rows:
        lines.append("")
    return parse_capture("\n".join(lines[:rows]), rows)


def same_style(a, b):
    return (a.fg == b.fg and a.bg == b.bg and a.bold == b.bold
            and a.underline == b.underline)


def draw_grid(draw, grid, x0, y0):
    for row_idx, row in enumerate(grid):
        c = 0
        while c < COLS:
            cell = row[c]
            e = c + 1
            while e < COLS and same_style(row[e], cell):
                e += 1
            bx = x0 + c * CELL_W
            by = y0 + row_idx * CELL_H
            draw.rectangle([bx, by, bx + (e - c) * CELL_W - 1,
                            by + CELL_H - 1], fill=cell.bg)
            text = "".join(row[i].ch for i in range(c, e))
            f = font_bold if cell.bold else font
            ty = by - FONT_TOP + max(0, (CELL_H - FONT_H) // 2)
            draw.text((bx, ty), text, font=f, fill=cell.fg)
            if cell.underline:
                draw.line([bx, by + CELL_H - 2, bx + (e - c) * CELL_W - 1,
                           by + CELL_H - 2], fill=cell.fg, width=1)
            c = e


def render_frame(bottom=False):
    top_rows = SPLIT_TOP_ROWS if bottom else (FULL_ROWS - 1)
    top = capture_pane(0, top_rows)

    img = Image.new("RGB", (W, H), (9, 11, 15))
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle(
        [PAD_X - 3, PAD_Y - 3, W - PAD_X + 3, H - PAD_Y + 3],
        radius=9, fill=WINDOW_BG, outline=BORDER, width=2)

    draw_grid(draw, top, PAD_X, PAD_Y)

    # Draw a single, evenly aligned divider through the code rows and the
    # statusline row, centered on the column where Neovim's pipe used to be.
    split_x = PAD_X + SPLIT_COL * CELL_W + 5
    draw.line([split_x, PAD_Y, split_x, PAD_Y + top_rows * CELL_H - 1],
              fill=SEPARATOR, width=2)

    if bottom:
        sep_y = PAD_Y + top_rows * CELL_H
        draw.rectangle([PAD_X, sep_y - 1, W - PAD_X, sep_y - 1],
                       fill=SEPARATOR)
        draw_grid(draw, capture_pane(1, SHELL_ROWS), PAD_X, sep_y)
    return img


INIT_LUA = """\
vim.opt.number = true
vim.opt.relativenumber = false
vim.opt.numberwidth = 2
vim.opt.termguicolors = true
vim.opt.laststatus = 2
vim.opt.showmode = false
vim.opt.signcolumn = "no"
vim.opt.cursorline = true
vim.opt.wrap = false
vim.opt.autoread = true
vim.opt.background = "dark"
vim.opt.statusline = " %f  %m  %=  c-autotmpl "

vim.cmd([[
highlight Normal        guifg=#d8dee9 guibg=#171a21
highlight LineNr        guifg=#4b5566 guibg=#171a21
highlight CursorLineNr  guifg=#e5c07b guibg=#222936
highlight CursorLine    guibg=#222936
highlight Comment       guifg=#7a9c7a gui=italic
highlight PreProc       guifg=#c678dd
highlight Define        guifg=#c678dd
highlight String        guifg=#e5c07b
highlight Type          guifg=#61afef
highlight Identifier    guifg=#d8dee9
highlight Function      guifg=#61afef
highlight Keyword       guifg=#c678dd
highlight Number        guifg=#d19a66
highlight Operator      guifg=#89ddff
highlight StatusLine    guifg=#d8dee9 guibg=#2b3242
highlight StatusLineNC  guifg=#8b94a7 guibg=#232936
highlight VertSplit     guifg=#3a4252 guibg=#171a21
highlight FoldColumn    guifg=#4b5566 guibg=#171a21
highlight NonText       guifg=#4b5566 guibg=#171a21
]])
"""

CONFIG = """\
dispatch=1
dispatch_truncate=1
dispatch_templates=Box,Container,Option,Result
preprocess_backend=local
"""

MAKEFILE = """\
check:
\tcc -I. -std=gnu11 -Wall -Wextra -Werror \\
\t\t-ftrack-macro-expansion=0 main.c -o demo && ./demo
"""

TEMPLATE_BOX = """\
// -- template type Box(T)
// -- +owner
#define BOX_CAT2_I(a, b) a##b
#define BOX_CAT2(a, b) BOX_CAT2_I(a, b)
#define BOX_CAT3_I(a, b, c) a##_##b##_##c
#define BOX_CAT3(a, b, c) BOX_CAT3_I(a, b, c)
#define Box(T) BOX_CAT2(Box_, T)
#define get_impl(T, Owner) BOX_CAT3(get, T, Owner)
typedef struct { T value; } Box(T);
static inline T get_impl(T, Owner)(Box(T) *b) { return b->value; }
#undef get_impl
"""

TEMPLATE_CONTAINER = """\
// -- template type Container(T)
// -- mangle +owner
#define CONTAINER_CAT2_I(a, b) a##b
#define CONTAINER_CAT2(a, b) CONTAINER_CAT2_I(a, b)
#define CONTAINER_CAT3_I(a, b, c) a##_##b##_##c
#define CONTAINER_CAT3(a, b, c) CONTAINER_CAT3_I(a, b, c)
#define Container(T) CONTAINER_CAT2(Container_, T)
#define get_impl(T, Owner) CONTAINER_CAT3(get, T, Owner)
typedef struct { T value; } Container(T);
static inline T get_impl(T, Owner)(Container(T) *c) { return c->value; }
#undef get_impl
"""

TEMPLATE_OPTION = """\
// -- template type Option(T)
#define OPTION_CAT2_I(a, b) a##b
#define OPTION_CAT2(a, b) OPTION_CAT2_I(a, b)
#define Option(T) OPTION_CAT2(Option_, T)
typedef struct { int has; T value; } Option(T);

// -- template function option_value_or(T)
#define OPTION_VALUE2_I(a, b) a##_##b
#define OPTION_VALUE2(a, b) OPTION_VALUE2_I(a, b)
#define option_value_or(T) OPTION_VALUE2(option_value_or, T)
static inline T option_value_or(T)(Option(T) value, T fallback) {
  return value.has ? value.value : fallback;
}

// -- template function option_map_impl(T, U, Owner)
// -- dispatch method Option.map explicit U
// -- +owner
#define OPTION_MAP4_I(a, b, c, d) a##_##b##_##c##_##d
#define OPTION_MAP4(a, b, c, d) OPTION_MAP4_I(a, b, c, d)
#define option_map_impl(T, U, Owner) OPTION_MAP4(map, T, U, Owner)
static inline Option(U) option_map_impl(T, U, Owner)(Option(T) value,
                                                     U (*f)(T)) {
  Option(U) out = {0};
  if (value.has) { out.has = 1; out.value = f(value.value); }
  return out;
}
#undef option_map_impl
"""

TEMPLATE_RESULT = """\
// -- template type Result(T, U)
#define RESULT_CAT3_I(a, b, c) a##_##b##_##c
#define RESULT_CAT3(a, b, c) RESULT_CAT3_I(a, b, c)
#define Result(T, U) RESULT_CAT3(Result, T, U)
typedef struct { int ok; T value; U error; } Result(T, U);

// -- template function result_value_or(T, U)
#define RESULT_VALUE3_I(a, b, c) a##_##b##_##c
#define RESULT_VALUE3(a, b, c) RESULT_VALUE3_I(a, b, c)
#define result_value_or(T, U) RESULT_VALUE3(result_value_or, T, U)
static inline T result_value_or(T, U)(Result(T, U) value, T fallback) {
  return value.ok ? value.value : fallback;
}

// -- template function result_map_impl(T, U, V, Owner)
// -- dispatch method Result.map explicit V
// -- mangle +owner
#define RESULT_MAP5_I(a, b, c, d, e) a##_##b##_##c##_##d##_##e
#define RESULT_MAP5(a, b, c, d, e) RESULT_MAP5_I(a, b, c, d, e)
#define result_map_impl(T, U, V, Owner) RESULT_MAP5(map, T, U, V, Owner)
static inline Result(V, U) result_map_impl(T, U, V, Owner)(Result(T, U) value,
                                                          V (*f)(T)) {
  Result(V, U) out = {0};
  if (value.ok) { out.ok = 1; out.value = f(value.value); }
  else { out.error = value.error; }
  return out;
}
#undef result_map_impl
"""

S1 = """\
#include <stdio.h>
#include "gen/box.h"
#include "gen/container.h"
#include "gen/dispatch.h"

int main(void) {
  Box(int) b = {7};
  Container(float) c = {2.5f};

  printf("%d %.1f\\n", 2 * get(&b), (double)get(&c));
  return 0;
}
"""

S2 = """\
#include <stdio.h>
#include "gen/box.h"
#include "gen/container.h"
#include "gen/option.h"
#include "gen/result.h"
#include "gen/dispatch.h"
#include "gen/option_value_or.h"
#include "gen/result_value_or.h"

static int twice(int x) { return 2 * x; }
static float half(int x) { return (float)x / 2.0f; }

int main(void) {
  Box(int) b = {7};
  Container(float) c = {2.5f};
  Option(int) o = {1, 7};
  Result(int, float) r = {1, 7, 0.0f};

  int x = 2 * get(&b);
  float y = get(&c);
  Option(int) oi = map(int)(o, twice);
  Option(float) of = map(float)(o, half);
  Result(int, float) ri = map(int)(r, twice);
  Result(float, float) rf = map(float)(r, half);

  printf("%d %.1f %d %.1f %d %.1f\\n",
         x, (double)y,
         option_value_or(int)(oi, -1),
         (double)option_value_or(float)(of, -1.0f),
         result_value_or(int, float)(ri, -1),
         (double)result_value_or(float, float)(rf, -1.0f));
  return 0;
}
"""


def choose_autotmpl(explicit):
    if explicit:
        candidate = Path(explicit).resolve()
        if not candidate.is_file():
            raise SystemExit(f"autotmpl binary not found: {candidate}")
        return candidate
    candidate = REPO / "build" / "autotmpl"
    if candidate.is_file():
        return candidate
    found = shutil.which("autotmpl")
    if found:
        return Path(found).resolve()
    raise SystemExit("build/autotmpl not found; run `make -B build/autotmpl` first")


def build_fixture(args):
    global FIXTURE, AUTOTMPL
    tmp_root = Path("/tmp/opencode")
    tmp_root = tmp_root if tmp_root.is_dir() else None
    FIXTURE = Path(tempfile.mkdtemp(prefix="c-autotmpl-record-",
                                    dir=str(tmp_root) if tmp_root else None))
    AUTOTMPL = choose_autotmpl(args.autotmpl)

    (FIXTURE / "tmpl").mkdir()
    (FIXTURE / "gen").mkdir()
    shutil.copy2(AUTOTMPL, FIXTURE / "autotmpl")
    (FIXTURE / "autotmpl").chmod(0o755)

    (FIXTURE / "init.lua").write_text(INIT_LUA)
    (FIXTURE / "autotmpl.conf").write_text(CONFIG)
    (FIXTURE / "Makefile").write_text(MAKEFILE)
    (FIXTURE / "tmpl" / "box.h").write_text(TEMPLATE_BOX)
    (FIXTURE / "tmpl" / "container.h").write_text(TEMPLATE_CONTAINER)
    (FIXTURE / "tmpl" / "option.h").write_text(TEMPLATE_OPTION)
    (FIXTURE / "tmpl" / "result.h").write_text(TEMPLATE_RESULT)
    (FIXTURE / "s1.c").write_text(S1)
    (FIXTURE / "s2.c").write_text(S2)
    (FIXTURE / "main.c").write_text(S1)
    for name in ("dispatch.h", "box.h", "container.h", "option.h", "result.h",
                 "option_value_or.h", "result_value_or.h",
                 "option_map_impl.h", "result_map_impl.h"):
        (FIXTURE / "gen" / name).write_text("")


def show_shell():
    global shell_visible
    if shell_visible:
        return
    tmux("split-window", "-v", "-l", str(SHELL_ROWS), "-t",
         pane_target(0), "-c", str(FIXTURE), "bash --noprofile --norc")
    tmux("send-keys", "-t", pane_target(1), "PS1='demo$ '", "Enter")
    tmux("send-keys", "-t", pane_target(1), "clear", "Enter")
    time.sleep(0.4)
    shell_visible = True


def hide_shell():
    global shell_visible
    if not shell_visible:
        return
    tmux("kill-pane", "-t", pane_target(1))
    time.sleep(0.4)
    shell_visible = False


def start_session():
    global shell_visible, WINDOW_INDEX, PANE_INDEX
    subprocess.run(["tmux", "kill-session", "-t", SESSION],
                   capture_output=True)
    tmux("new-session", "-d", "-s", SESSION, "-x", str(COLS), "-y",
         str(FULL_ROWS), "-c", str(FIXTURE),
         "nvim -u init.lua -c 'botright vsplit gen/dispatch.h' -c 'wincmd h' main.c")
    WINDOW_INDEX = tmux("display-message", "-p", "-t", SESSION,
                        "#{window_index}").strip()
    PANE_INDEX = tmux("display-message", "-p", "-t", SESSION,
                      "#{pane_index}").strip()
    tmux("set-option", "-t", SESSION, "status", "off")
    time.sleep(1.0)
    shell_visible = False


def send(pane, keys, literal=False):
    args = ["send-keys", "-t", pane_target(pane)]
    if literal:
        args.append("-l")
    args.extend(["--", keys])
    tmux(*args)


def line_of(rel, needle):
    for number, line in enumerate((FIXTURE / rel).read_text().splitlines(), 1):
        if needle in line:
            return number
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", action="store_true",
                        help="write a single PNG preview instead of the GIF")
    parser.add_argument("--output", default=str(REPO / "assets" / "demo.gif"),
                        help="GIF destination (default: assets/demo.gif)")
    parser.add_argument("--autotmpl", help="path to a specific autotmpl binary")
    parser.add_argument("--keep-fixture", action="store_true",
                        help="keep the generated temporary fixture")
    args = parser.parse_args()

    build_fixture(args)
    try:
        start_session()
        if args.preview:
            preview = Path("/tmp/opencode/demo-preview.png")
            preview.parent.mkdir(parents=True, exist_ok=True)
            render_frame().save(preview)
            print(f"preview written: {preview}")
            print(f"fixture: {FIXTURE}")
            return

        frames = []
        delays = []

        def shot(delay, bottom=False):
            frames.append(render_frame(bottom))
            delays.append(delay)

        def hold(delay, bottom=False):
            shot(delay, bottom)

        def type_with_frames(text, chunk=6, bottom=False):
            for i in range(0, len(text), chunk):
                send(1, text[i:i + chunk], literal=True)
                time.sleep(0.02)
                shot(8, bottom)

        def run_command():
            send(1, "Enter")
            time.sleep(0.55)

        def enter_and_wait():
            send(0, "Enter")
            time.sleep(0.5)

        def go_left():
            send(0, "Escape")
            send(0, "C-w")
            send(0, "h")
            time.sleep(0.15)

        def go_right():
            send(0, "Escape")
            send(0, "C-w")
            send(0, "l")
            time.sleep(0.15)

        def reload_and_top():
            go_left()
            send(0, "Escape")
            time.sleep(0.03)
            send(0, ":e!", literal=True)
            enter_and_wait()
            go_right()
            send(0, "Escape")
            time.sleep(0.03)
            send(0, ":e!", literal=True)
            enter_and_wait()
            send(0, "gg")
            time.sleep(0.25)

        def set_right(path):
            go_right()
            send(0, "Escape")
            time.sleep(0.03)
            send(0, ":e " + path, literal=True)
            shot(10)
            enter_and_wait()
            send(0, "gg")
            time.sleep(0.15)

        def goto_right_line(number):
            go_right()
            send(0, "Escape")
            time.sleep(0.03)
            send(0, ":" + str(number), literal=True)
            shot(10)
            enter_and_wait()

        def command_sequence(text, chunk, after_delay):
            show_shell()
            type_with_frames(text, chunk, bottom=True)
            run_command()
            hold(after_delay, bottom=True)
            hide_shell()

        # Intro: same-arity get() calls beside an empty central dispatch header.
        hold(100)

        # Owner-mangled ordinary targets for Box(int) and Container(float).
        command_sequence(
            "rm -f gen/*.h && cp s1.c main.c && ./autotmpl --root . main.c",
            5, 110)
        reload_and_top()
        hold(130)

        set_right("gen/dispatch.h")
        goto_right_line(line_of("gen/dispatch.h", "dispatch begin shared.get"))
        hold(130)

        set_right("tmpl/box.h")
        goto_right_line(line_of("tmpl/box.h", "// -- +owner"))
        hold(110)

        set_right("tmpl/container.h")
        goto_right_line(line_of("tmpl/container.h", "mangle +owner"))
        hold(110)

        # Add Option and Result map(V) methods with explicit result types.
        command_sequence(
            "rm -f gen/*.h && cp s2.c main.c && ./autotmpl --root . main.c",
            5, 130)
        reload_and_top()
        hold(150)

        set_right("gen/dispatch.h")
        goto_right_line(line_of("gen/dispatch.h", "dispatch begin shared.map"))
        hold(170)

        set_right("tmpl/option.h")
        goto_right_line(line_of("tmpl/option.h", "dispatch method Option.map"))
        hold(120)

        set_right("tmpl/result.h")
        goto_right_line(line_of("tmpl/result.h", "dispatch method Result.map"))
        hold(130)

        # Bottom panel pops up to compile and run the recorded source.
        show_shell()
        type_with_frames("make check", 3, bottom=True)
        run_command()
        hold(280, bottom=True)
        hide_shell()
        hold(130)

        # Build a shared palette that keeps the syntax colors intact. Downsampling
        # frames used to average them away; antialiased edge shades are mapped to
        # the nearest of these base colors instead.
        theme_colors = [
            DEFAULT_FG, DEFAULT_BG, WINDOW_BG, BORDER, SEPARATOR,
            (229, 192, 123),  # yellow: numbers / strings / line numbers
            (198, 120, 221),  # purple: preprocessor / keywords
            (97, 175, 239),   # blue: types / functions
            (122, 156, 122),  # green: comments
            (209, 154, 102),  # orange: numbers
            (137, 221, 255),  # cyan: operators
            (75, 85, 102),    # inactive line numbers / dim text
            (139, 148, 167),  # inactive statusline
            (43, 50, 66),     # active statusline
            (35, 41, 54),     # inactive statusline background
            (34, 41, 54),     # cursorline background
            (224, 226, 234),  # bold return keyword
        ]
        counts = Counter()
        for frame in frames:
            for count, color in frame.getcolors(maxcolors=1 << 24):
                counts[color] += count
        palette_colors = list(dict.fromkeys(theme_colors))
        for color, _ in counts.most_common():
            if color not in palette_colors:
                palette_colors.append(color)
            if len(palette_colors) == 256:
                break
        palette_img = Image.new("P", (1, 1))
        palette = []
        for color in palette_colors:
            palette.extend(color)
        palette.extend([0, 0, 0] * (256 - len(palette_colors)))
        palette_img.putpalette(palette)
        out_frames = [f.quantize(palette=palette_img, dither=Image.Dither.NONE)
                      for f in frames]

        dest = Path(args.output).resolve()
        dest.parent.mkdir(parents=True, exist_ok=True)
        out_frames[0].save(
            dest, save_all=True, append_images=out_frames[1:],
            duration=[d * 10 for d in delays], loop=0, optimize=True,
            disposal=2)
        print(f"wrote {dest}: {len(frames)} frames, {sum(delays) / 100:.1f}s")
    finally:
        subprocess.run(["tmux", "kill-session", "-t", SESSION],
                       capture_output=True)
        if not args.keep_fixture:
            shutil.rmtree(FIXTURE, ignore_errors=True)


if __name__ == "__main__":
    main()
