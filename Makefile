BUILD_DIR ?= build
BUILD_TYPE ?= Debug
CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || echo 1)

.PHONY: all configure build test python-test check clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

python-test:
	python3 -m unittest discover -s tests -v

check: test python-test

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)
