# dasdl — a DASDL front end for the PC.
#
# The parser in parser/dasdl.cpp was generated from grammar/dasdl.graph by
# Trackway, which is not distributed here. So this Makefile builds and runs the
# compiler; it does not regenerate it.
#
# Three directories, three roles: parser/ and properties/ are source and are
# tracked, build/ holds the binary, out/ holds what compiling a description
# writes. The last two are generated in full and safe to delete.

CXX      := g++
# -Wno-unused-label: the generated parser emits an exit label per entity and
# only jumps to some of them. Not a defect in the grammar, and not worth 100
# lines of noise on a clean build.
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-label -O2

SRC     := parser/dasdl.cpp
BIN     := build/dasdl
OUTDIR  := out
WEBDIR  := build/web
# The browser build only. em++ is Emscripten's C++ driver (emcc is the C one
# and will not link libc++), and Emscripten is itself a Python program, so
# asking for python3 here costs nothing that em++ has not already asked for.
EMXX    ?= em++
PYTHON  ?= python3
SAMPLES := $(wildcard samples/*.dasdl)

.PHONY: help build compile web site clean
.DEFAULT_GOAL := help

help:
	@echo "  make build    Build build/dasdl from parser/dasdl.cpp"
	@echo "  make compile  Build, then compile every sample into a model and a schema"
	@echo "  make web      Build the browser page (needs Emscripten; nothing else does)"
	@echo "  make site     Stage the page as build/site/index.html, for a static host"
	@echo "  make clean    Remove the binary and everything a compile wrote"

# Carries a recipe of its own so that the target names what it produced, rather
# than answering "Nothing to be done for 'build'" and reading like a failure.
build: $(BIN)
	@echo "$(BIN) ready"

# build/ and out/ are ignored in full, so a fresh clone has neither: what
# writes into them creates them first.
$(BIN): $(SRC) properties/properties.hpp properties/dasdl_model.hpp properties/dasdl_sql.hpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

# Reads a description and writes both of its outputs itself: the model, and the
# MariaDB schema written from that model in memory.
compile: $(BIN)
	@mkdir -p $(OUTDIR)
	@for f in $(SAMPLES); do \
	    b=$$(basename $$f .dasdl); \
	    echo "--- $$b"; \
	    ./$(BIN) -s "$$f" -o "$(OUTDIR)/$$b" || exit 1; \
	done

# The same compiler, for a reader with no toolchain: one self-contained HTML
# file with the WebAssembly build inlined, which fetches nothing when opened.
web: $(WEBDIR)/dasdl.html

$(WEBDIR)/dasdl.js: $(SRC) properties/properties.hpp properties/dasdl_model.hpp properties/dasdl_sql.hpp
	@mkdir -p $(WEBDIR)
	$(EMXX) $(CXXFLAGS) $(SRC) -o $@ \
	    -sSINGLE_FILE=1 -sMODULARIZE=1 -sEXPORT_NAME=createDasdl \
	    -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM=1 \
	    -sEXPORTED_RUNTIME_METHODS=callMain,FS -sALLOW_MEMORY_GROWTH=1 \
	    -sENVIRONMENT=web

$(WEBDIR)/dasdl.html: $(WEBDIR)/dasdl.js web/dasdl.html.in web/fonts.css web/bundle.py $(SAMPLES)
	$(PYTHON) web/bundle.py $(WEBDIR)

# What a static host wants: one directory holding one index.html. The page is
# self-contained, so this is the whole site.
site: $(WEBDIR)/dasdl.html
	@mkdir -p build/site
	cp $(WEBDIR)/dasdl.html build/site/index.html
	@echo "build/site/index.html ready — $$(du -h build/site/index.html | cut -f1)"

clean:
	rm -rf build $(OUTDIR)
