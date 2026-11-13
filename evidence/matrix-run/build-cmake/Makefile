# Makefile — wraps CMake configure / build / test / install
# Usage:
#   make            — configure (Debug) + build
#   make test       — run the test suite
#   make release    — configure (Release) + build
#   make install    — install to $(PREFIX)
#   make clean      — remove the build directory
#   make distclean  — remove build + any generated files

BUILD_DIR  ?= build
PREFIX     ?= $(HOME)/.local
BUILD_TYPE ?= Debug
CMAKE      ?= cmake
CTEST      ?= ctest
JOBS       ?= $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

.PHONY: all configure build test release install clean distclean

# ── Default target ────────────────────────────────────────────────────────────
all: build

# ── Configure ─────────────────────────────────────────────────────────────────
configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
		-DMATHLIB_BUILD_TESTS=ON

# ── Build ─────────────────────────────────────────────────────────────────────
build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

# ── Test ──────────────────────────────────────────────────────────────────────
test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --parallel $(JOBS)

# ── Release ───────────────────────────────────────────────────────────────────
release:
	$(MAKE) BUILD_TYPE=Release build

# ── Install ───────────────────────────────────────────────────────────────────
install: build
	$(CMAKE) --install $(BUILD_DIR)

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean:
	rm -rf $(BUILD_DIR)
