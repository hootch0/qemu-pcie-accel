# SPDX-License-Identifier: MIT
#
# PCIe Accelerator Device - Examples Makefile (io_uring)
#
# Build:
#   make                  - Build all examples
#   make clean            - Clean build artifacts
#   make install          - Install examples to PREFIX/bin
#
# Dependencies:
#   liburing-dev (Debian/Ubuntu) or liburing-devel (Fedora/RHEL)
#   Install: apt install liburing-dev  OR  dnf install liburing-devel
#

# Compiler settings
CC ?= gcc
CFLAGS := -Wall -Wextra -O2
CFLAGS += -D_GNU_SOURCE

# Link with liburing for io_uring support
LIBS := -luring

# Debug build
ifdef DEBUG
CFLAGS += -g -DDEBUG -O0
endif

# Installation
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

# Library path for running
LIBDIR := $(realpath ../libaccel)

# Source files
EXAMPLES := test_loopback test_p2p test_cxl

# Default target
all: $(EXAMPLES)

# Build each example
test_loopback: test_loopback.c ../libaccel/libaccel.a
	$(CC) $(CFLAGS) -I../libaccel -o $@ $< ../libaccel/libaccel.a $(LIBS)

test_p2p: test_p2p.c ../libaccel/libaccel.a
	$(CC) $(CFLAGS) -I../libaccel -o $@ $< ../libaccel/libaccel.a $(LIBS)

test_cxl: test_cxl.c ../libaccel/libaccel.a
	$(CC) $(CFLAGS) -I../libaccel -o $@ $< ../libaccel/libaccel.a $(LIBS)

# Ensure library is built
../libaccel/libaccel.a:
	$(MAKE) -C ../libaccel

# Clean
clean:
	rm -f $(EXAMPLES) *.o

# Install
install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(EXAMPLES) $(DESTDIR)$(BINDIR)/

# Uninstall
uninstall:
	for prog in $(EXAMPLES); do \
		rm -f $(DESTDIR)$(BINDIR)/$$prog; \
	done

# Run tests (requires device and root)
run-loopback: test_loopback
	@echo "Running loopback test..."
	@echo "Note: Requires /dev/accel0 and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_loopback

run-loopback-async: test_loopback
	@echo "Running loopback test (async batch mode)..."
	@echo "Note: Requires /dev/accel0 and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_loopback -a -b 32

run-loopback-concurrent: test_loopback
	@echo "Running loopback test (concurrent async mode)..."
	@echo "Note: Requires /dev/accel0 and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_loopback -c -b 16

run-p2p: test_p2p
	@echo "Running P2P test..."
	@echo "Note: Requires /dev/accel0, /dev/accel1 and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_p2p

run-p2p-async: test_p2p
	@echo "Running P2P test (concurrent async mode)..."
	@echo "Note: Requires /dev/accel0, /dev/accel1 and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_p2p -a -c 4

run-p2p-bidir: test_p2p
	@echo "Running P2P test (bidirectional async mode)..."
	@echo "Note: Requires /dev/accel0, /dev/accel1 and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_p2p -b

run-cxl: test_cxl
	@echo "Running CXL memory test..."
	@echo "Note: Requires /dev/accel0 with CXL memory and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_cxl

run-cxl-all: test_cxl
	@echo "Running CXL memory test (all tests)..."
	@echo "Note: Requires /dev/accel0 with CXL memory and root privileges"
	sudo LD_LIBRARY_PATH=$(LIBDIR) ./test_cxl -a

# Help
help:
	@echo "PCIe Accelerator Examples (io_uring)"
	@echo ""
	@echo "Targets:"
	@echo "  all                  - Build all examples (default)"
	@echo "  clean                - Remove build artifacts"
	@echo "  install              - Install to $(BINDIR)"
	@echo "  uninstall            - Remove from $(BINDIR)"
	@echo ""
	@echo "Loopback Tests:"
	@echo "  run-loopback         - Run loopback (synchronous)"
	@echo "  run-loopback-async   - Run loopback (async batch mode)"
	@echo "  run-loopback-concurrent - Run loopback (concurrent async)"
	@echo ""
	@echo "P2P Tests:"
	@echo "  run-p2p              - Run P2P test (synchronous)"
	@echo "  run-p2p-async        - Run P2P test (concurrent async)"
	@echo "  run-p2p-bidir        - Run P2P test (bidirectional)"
	@echo ""
	@echo "CXL Tests:"
	@echo "  run-cxl              - Run CXL memory test (basic)"
	@echo "  run-cxl-all          - Run CXL memory test (all tests)"
	@echo ""
	@echo "Examples:"
	@echo "  test_loopback - Basic loopback DMA test with io_uring"
	@echo "  test_p2p      - Peer-to-peer DMA test with io_uring"
	@echo "  test_cxl      - CXL memory read/write test"
	@echo ""
	@echo "Usage:"
	@echo "  ./test_loopback -h       # Show help"
	@echo "  ./test_loopback -a       # Async batch mode"
	@echo "  ./test_loopback -c       # Concurrent async mode"
	@echo "  ./test_p2p -h            # Show help"
	@echo "  ./test_p2p -a            # Concurrent async mode"
	@echo "  ./test_p2p -b            # Bidirectional async mode"
	@echo "  ./test_cxl -h            # Show help"
	@echo "  ./test_cxl -a            # Run all tests"
	@echo ""
	@echo "Dependencies:"
	@echo "  liburing-dev (Debian/Ubuntu) or liburing-devel (Fedora/RHEL)"

.PHONY: all clean install uninstall help
.PHONY: run-loopback run-loopback-async run-loopback-concurrent
.PHONY: run-p2p run-p2p-async run-p2p-bidir
.PHONY: run-cxl run-cxl-all
