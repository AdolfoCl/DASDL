# dasdl — a DASDL front end for the PC.
#
# The parser in build/dasdl.cpp was generated from grammar/dasdl.graph by
# Trackway, which is not distributed here. So this Makefile builds and runs the
# compiler; it does not regenerate it.

CXX      := g++
# -Wno-unused-label: the generated parser emits an exit label per entity and
# only jumps to some of them. Not a defect in the grammar, and not worth 100
# lines of noise on a clean build.
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-label -O2

SRC     := build/dasdl.cpp
BIN     := build/dasdl
SAMPLES := $(wildcard samples/*.dasdl)

.PHONY: help build compile clean
.DEFAULT_GOAL := help

help:
	@echo "  make build    Build build/dasdl from build/dasdl.cpp"
	@echo "  make compile  Build, then compile every sample into a model and a schema"
	@echo "  make clean    Remove the binary and everything a compile wrote"

build: $(BIN)

$(BIN): $(SRC) properties/properties.hpp properties/dasdl_model.hpp properties/dasdl_sql.hpp
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

# Reads a description and writes both of its outputs itself: the model, and the
# MariaDB schema written from that model in memory.
compile: build
	@for f in $(SAMPLES); do \
	    b=$$(basename $$f .dasdl); \
	    echo "--- $$b"; \
	    ./$(BIN) -s "$$f" -o "build/$$b" || exit 1; \
	done

clean:
	rm -f $(BIN) build/*.model.json build/*.sql
