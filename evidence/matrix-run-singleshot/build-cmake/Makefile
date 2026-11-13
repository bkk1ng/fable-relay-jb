# Makefile — thin wrapper around CMake configure / build / test / install.
#
# Targets:
#   make            build (default)
#   make configure   run cmake configure
#   make build       compile the project
#   make test        run the test suite via ctest
#   make install     install to CMAKE_INSTALL_PREFIX (default /usr/local)
#   make clean       remove the entire build directory
#   make rebuild     clean + build

BUILD_DIR  := build
CMAKE      ?= cmake
CTEST      ?= ctest
INSTALL_PREFIX ?= /usr/local

.PHONY: all configure build test install clean rebuild

all: build

configure:
	$(CMAKE) -B $(BUILD_DIR) -S . -DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

install: build
	$(CMAKE) --install $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build
