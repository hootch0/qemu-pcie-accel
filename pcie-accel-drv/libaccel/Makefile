# SPDX-License-Identifier: MIT
#
# PCIe Accelerator Device - User Mode Library Makefile (io_uring)
#
# Build:
#   make                  - Build the library
#   make clean            - Clean build artifacts
#   make install          - Install to system
#   make uninstall        - Remove from system
#
# Dependencies:
#   liburing-dev (Debian/Ubuntu) or liburing-devel (Fedora/RHEL)
#   Install: apt install liburing-dev  OR  dnf install liburing-devel
#

LIB_NAME := libaccel
VERSION_MAJOR := 2
VERSION_MINOR := 0
VERSION_PATCH := 0

# Compiler settings
CC ?= gcc
CFLAGS := -Wall -Wextra -Werror -fPIC -O2
CFLAGS += -D_GNU_SOURCE
LDFLAGS := -shared
LIBS := -luring

# Debug build
ifdef DEBUG
CFLAGS += -g -DDEBUG -O0
endif

# Installation directories
PREFIX ?= /usr/local
LIBDIR := $(PREFIX)/lib
INCLUDEDIR := $(PREFIX)/include

# Source files
SRCS := libaccel.c
OBJS := $(SRCS:.c=.o)
HDRS := libaccel.h

# Library names
LIB_STATIC := $(LIB_NAME).a
LIB_SHARED := $(LIB_NAME).so
LIB_SONAME := $(LIB_SHARED).$(VERSION_MAJOR)
LIB_FULL := $(LIB_SHARED).$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Default target
all: $(LIB_STATIC) $(LIB_FULL)

# Static library
$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

# Shared library
$(LIB_FULL): $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-soname,$(LIB_SONAME) -o $@ $^
	ln -sf $(LIB_FULL) $(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(LIB_SHARED)

# Object files
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean
clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) $(LIB_SONAME) $(LIB_FULL)

# Install
install: all
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(LIB_FULL) $(DESTDIR)$(LIBDIR)/
	ln -sf $(LIB_FULL) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/$(LIB_SHARED)
	install -m 644 $(HDRS) $(DESTDIR)$(INCLUDEDIR)/
	ldconfig $(DESTDIR)$(LIBDIR) 2>/dev/null || true

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_STATIC)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_FULL)
	rm -f $(DESTDIR)$(INCLUDEDIR)/libaccel.h
	ldconfig $(DESTDIR)$(LIBDIR) 2>/dev/null || true

# Generate pkg-config file
pkgconfig: libaccel.pc

libaccel.pc:
	@echo "prefix=$(PREFIX)" > $@
	@echo "libdir=\$${prefix}/lib" >> $@
	@echo "includedir=\$${prefix}/include" >> $@
	@echo "" >> $@
	@echo "Name: libaccel" >> $@
	@echo "Description: PCIe Accelerator Device User Library" >> $@
	@echo "Version: $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)" >> $@
	@echo "Libs: -L\$${libdir} -laccel" >> $@
	@echo "Cflags: -I\$${includedir}" >> $@

.PHONY: all clean install uninstall pkgconfig
