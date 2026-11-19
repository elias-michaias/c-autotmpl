# Build the clangd feature module in src/plugin/ as a dlopen-able shared
# library.
#
# The plugin intentionally links no LLVM/clangd libraries: all symbols are
# resolved from the clangd process that loads it (this is how LLVM plugins
# built with `llvm_add_library(MODULE ... PLUGIN_TOOL clangd)` work).
#
# The standalone CLI in src/cli/ is separate: plain C99, no LLVM, no libclang,
# no C++ runtime. It is the default `build/autotmpl` and the binary installed
# by `make install`.

LLVM_BUILD ?= /home/elias/.local/build/llvm-main-plugin
LLVM_SRC   ?= /home/elias/.local/src/llvm-project-main
CLANGD_SRC ?= $(LLVM_SRC)/clang-tools-extra
LLVM_CONFIG ?= $(LLVM_BUILD)/bin/llvm-config
# Must match the _GLIBCXX_USE_CXX11_ABI of the clangd being loaded into.
LLVM_ABI ?= 1
LLVM_INCDIR := $(shell $(LLVM_CONFIG) --includedir)
LLVM_LIBDIR := $(shell $(LLVM_CONFIG) --libdir)

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PORTABLE_CFLAGS := -std=c99 -pedantic-errors $(CFLAGS)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -fPIC -fno-exceptions -fno-rtti \
             -fvisibility-inlines-hidden -fno-semantic-interposition \
             -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
             -Wno-comment \
             -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS \
             -D__STDC_LIMIT_MACROS -D_GLIBCXX_USE_CXX11_ABI=$(LLVM_ABI)

INCLUDES := \
  -I$(CLANGD_SRC)/clangd \
  -I$(CLANGD_SRC)/include-cleaner/include \
  -I$(LLVM_BUILD)/tools/clang/tools/extra/clangd \
  -I$(LLVM_SRC)/clang/include \
  -I$(LLVM_BUILD)/tools/clang/include \
  -I$(LLVM_BUILD)/include \
  -I$(LLVM_SRC)/llvm/include

PLUGIN_HDRS := $(wildcard src/plugin/*.h)
PLUGIN_CORE_SRC := src/plugin/Instantiator.cpp src/plugin/SourceScan.cpp \
                   src/plugin/TemplateRegistry.cpp src/plugin/TypeIndex.cpp
PLUGIN_SRC := src/plugin/AutotmplModule.cpp $(PLUGIN_CORE_SRC)
PLUGIN_OBJ := $(patsubst src/plugin/%.cpp,build/plugin/%.o,$(PLUGIN_SRC))
PLUGIN := build/libc-autotmpl.so

# Portable standalone CLI (installed by default). It is one C99 source file.
CLI_SRC := src/cli/autotmpl.c
CLI_OBJ := $(patsubst src/cli/%.c,build/cli/%.o,$(CLI_SRC))
CLI := build/autotmpl

SELFTEST := build/c-autotmpl-selftest

.PHONY: all clean install selftest lsp-check standalone-check test test-check \
        bench portability-check

all: $(PLUGIN) $(CLI)

build:
	mkdir -p build

build/plugin:
	mkdir -p build/plugin

build/cli:
	mkdir -p build/cli

build/plugin/%.o: src/plugin/%.cpp $(PLUGIN_HDRS) | build/plugin
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Isrc/plugin -c $< -o $@

build/cli/%.o: src/cli/%.c | build/cli
	$(CC) $(PORTABLE_CFLAGS) -c $< -o $@

$(PLUGIN): $(PLUGIN_OBJ)
	$(CXX) -shared -o $@ $^
	@echo "Built $@"

$(CLI): $(CLI_OBJ)
	$(CC) $(PORTABLE_CFLAGS) -o $@ $^
	@echo "Built $@ (portable C99, no LLVM/libclang)"

portability-check: $(CLI)
	@if command -v ldd >/dev/null 2>&1 && \
	   ldd $(CLI) | grep -Eiq 'LLVM|clang|stdc\+\+'; then \
	     echo "$(CLI) links an LLVM/clang/C++ runtime" >&2; \
	     ldd $(CLI) >&2; \
	     exit 1; \
	 fi
	@echo "$(CLI) has no LLVM/libclang/C++ runtime dependency"

# Install into a user-writable location and print usage flags.
install: $(PLUGIN) $(CLI)
	install -d $(HOME)/.local/bin
	install -d $(HOME)/.local/lib/c-autotmpl
	install -m 755 $(PLUGIN) $(HOME)/.local/lib/c-autotmpl/
	install -m 755 $(CLI) $(HOME)/.local/bin/autotmpl
	@echo "clangd: clangd --load=$(HOME)/.local/lib/c-autotmpl/libc-autotmpl.so ..."
	@echo "standalone: $(HOME)/.local/bin/autotmpl <source-files> ..."

# Plugin-core registry/instantiator checks.
$(SELFTEST): src/plugin/selftest.cpp $(PLUGIN_CORE_SRC) $(PLUGIN_HDRS) | build
	$(CXX) -std=c++17 -fno-exceptions -fno-rtti -I$(LLVM_INCDIR) -Isrc/plugin \
	  $< $(PLUGIN_CORE_SRC) -L$(LLVM_LIBDIR) -lLLVM \
	  -Wl,-rpath,$(LLVM_LIBDIR) -o $@
	$@

selftest: $(SELFTEST)

# Real LSP smoke test. clangd --check does not run AST feature modules, so a
# live clangd session is needed to exercise the plugin.
lsp-check: $(PLUGIN)
	python3 tools/lsp-check.py $(abspath $(PLUGIN))

standalone-check: $(CLI)
	python3 tools/standalone-check.py $(abspath $(CLI))

test: selftest lsp-check standalone-check portability-check

bench: $(CLI) $(PLUGIN)
	python3 tools/bench.py $(abspath $(CLI)) $(abspath $(PLUGIN)) $(BENCH_ARGS)

# Kept as an alias for older docs/scripts.
test-check: lsp-check

clean:
	rm -rf build
