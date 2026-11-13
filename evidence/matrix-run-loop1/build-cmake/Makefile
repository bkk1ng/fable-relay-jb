BUILD_DIR  ?= build
BUILD_TYPE ?= Debug
INSTALL_PREFIX ?= $(HOME)/.local
JOBS       ?= $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

CMAKE      := cmake
CTEST      := ctest

.PHONY: all configure build test install clean distclean help

##
## Default target: configure + build
##
all: build

## ── Configure ────────────────────────────────────────────────────────────────
configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX)

## ── Build ────────────────────────────────────────────────────────────────────
build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

## ── Test ─────────────────────────────────────────────────────────────────────
test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --parallel $(JOBS)

## ── Install ──────────────────────────────────────────────────────────────────
install: build
	$(CMAKE) --install $(BUILD_DIR)

## ── Clean targets ────────────────────────────────────────────────────────────
clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean:
	rm -rf $(BUILD_DIR)

## ── Help ─────────────────────────────────────────────────────────────────────
help:
	@echo "Targets:"
	@echo "  all          configure + build (default)"
	@echo "  configure    run cmake -S . -B \$(BUILD_DIR)"
	@echo "  build        compile the library and tests"
	@echo "  test         run the test suite via ctest"
	@echo "  install      install to \$(INSTALL_PREFIX)  [default: ~/.local]"
	@echo "  clean        remove compiled objects"
	@echo "  distclean    remove the entire build directory"
	@echo ""
	@echo "Variables (override on the command line):"
	@echo "  BUILD_DIR    $(BUILD_DIR)"
	@echo "  BUILD_TYPE   $(BUILD_TYPE)   (Debug | Release | RelWithDebInfo)"
	@echo "  INSTALL_PREFIX $(INSTALL_PREFIX)"
	@echo "  JOBS         $(JOBS)"
