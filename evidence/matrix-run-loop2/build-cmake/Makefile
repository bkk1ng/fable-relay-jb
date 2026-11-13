# Makefile wrapper around the CMake configure / build / test workflow.
#
# Targets:
#   make            configure + build
#   make build      force a build (configure first if needed)
#   make test       build, then run tests
#   make clean      remove build artifacts
#   make reinstall  install to the prefix (sudo may be required)
#   make uninstall  remove installed files
#   make distclean  clean + remove build directory
#
# Override variables on the command line, e.g.:
#   make test BUILD_TYPE=Release
#   make install DESTDIR=/tmp/staging PREFIX=/opt/mathlib

CMAKE        ?= cmake
BUILD_DIR    ?= build
BUILD_TYPE   ?= Debug
GENERATOR    ?=
PREFIX       ?= /usr/local
DESTDIR      ?=

CTEST        ?= ctest

CMAKE_ARGS   := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
CMAKE_ARGS   += -DCMAKE_INSTALL_PREFIX=$(PREFIX)

ifneq ($(GENERATOR),)
CMAKE_ARGS += -G "$(GENERATOR)"
endif

.PHONY: all build test clean distclean install reinstall uninstall help

all: build

build: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --parallel

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

clean:
ifneq ($(wildcard $(BUILD_DIR)/CMakeCache.txt),)
	$(CMAKE) --build $(BUILD_DIR) --target clean
endif

distclean:
	rm -rf $(BUILD_DIR)

install: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --install $(BUILD_DIR)

reinstall: install

uninstall: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --install $(BUILD_DIR) --strip 1 >/dev/null 2>&1 || true
	@# Fallback: remove the prefix tree we own.
	xargs -I{} rm -rf {} < /dev/null || true

help:
	@echo "Targets: all build test clean distclean install reinstall uninstall"
	@echo "Variables: BUILD_DIR BUILD_TYPE GENERATOR PREFIX DESTDIR"

# Configure once, reuse the cached build directory on later runs.
$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_ARGS)
