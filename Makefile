# SPDX-License-Identifier: MIT
#
# Build the wl_screencopy capture library and install it where the Python
# package expects to find it. No root required.
#
#   make            build libwl_screencopy.so + drop a copy in the package
#   make test       unit tests + integration tests (needs a wlroots compositor)
#   make install    cp to $(PREFIX)/lib, $(PREFIX)/include
#   make clean

VERSION      := 0.1.0
SOVERSION    := 0
PREFIX       ?= /usr/local
PY           ?= python3

CC           ?= cc
PKG_CONFIG   ?= pkg-config
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null)
ifeq ($(WAYLAND_SCANNER),)
WAYLAND_SCANNER := $(shell command -v wayland-scanner 2>/dev/null)
endif

BUILD        := build
GEN          := $(BUILD)/gen
LIB_NAME     := libwl_screencopy.so
LIB          := $(BUILD)/$(LIB_NAME).$(VERSION)
NATIVE_DIR   := python/wl_screencopy/_native
NATIVE       := $(NATIVE_DIR)/$(LIB_NAME)

WL_CFLAGS    := $(shell $(PKG_CONFIG) --cflags wayland-client)
WL_LIBS      := $(shell $(PKG_CONFIG) --libs wayland-client)

WARN         := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wwrite-strings \
                -Wno-missing-field-initializers
CFLAGS       ?= -O3 -g
ALL_CFLAGS   := -std=c11 -D_GNU_SOURCE -fPIC $(CFLAGS) $(WARN) \
                -Iinclude -Isrc -I$(GEN) $(WL_CFLAGS)
LDFLAGS      ?=
ALL_LDFLAGS  := $(LDFLAGS) $(WL_LIBS) -lm

# Vendored protocol XMLs (see proto/PROVENANCE.md).
LIB_PROTOS   := proto/wlr-screencopy-unstable-v1.xml \
                proto/xdg-output-unstable-v1.xml
# Only needed by the test client that paints a known pattern.
TEST_PROTOS  := proto/xdg-shell.xml
GEN_H        := $(patsubst proto/%.xml,$(GEN)/%-client-protocol.h, \
                 $(LIB_PROTOS) $(TEST_PROTOS))
GEN_C        := $(patsubst proto/%.xml,$(GEN)/%-protocol.c,$(LIB_PROTOS))
GEN_TEST_C   := $(patsubst proto/%.xml,$(GEN)/%-protocol.c,$(TEST_PROTOS))

SRCS         := src/session.c src/capture.c src/convert.c src/abi.c
# All intermediates live under build/ so the source tree stays clean.
OBJS         := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS)) $(GEN_C:.c=.o)

.SECONDARY: $(GEN_C) $(GEN_H)

.PHONY: all native clean test unittest integration tools install uninstall info

all: native tools info

info: $(LIB)
	@echo "built $(LIB)"
	@echo "      scanner: $(WAYLAND_SCANNER)"
	@echo "      python package will load: $(NATIVE)"

# ---- protocol code generation -------------------------------------------

$(GEN)/%-client-protocol.h: proto/%.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@

$(GEN)/%-protocol.c: proto/%.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(GEN):
	mkdir -p $(GEN)

# Make sure the generated headers exist before any source is compiled.
$(OBJS): $(GEN_H)

# ---- library -------------------------------------------------------------

$(BUILD)/obj/%.o: src/%.c | $(GEN)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(CC) -shared -Wl,-soname,$(LIB_NAME).$(SOVERSION) -o $@ $(OBJS) $(ALL_LDFLAGS)
	cd $(BUILD) && ln -sf $(LIB_NAME).$(VERSION) $(LIB_NAME).$(SOVERSION) && \
	               ln -sf $(LIB_NAME).$(VERSION) $(LIB_NAME)

# The package loads this by absolute path (dlopen), so one real file is
# enough - no SONAME symlinks, which would each be dereferenced into the
# wheel as a full copy of the library.
native: $(LIB)
	@mkdir -p $(NATIVE_DIR)
	@rm -f $(NATIVE_DIR)/$(LIB_NAME) $(NATIVE_DIR)/$(LIB_NAME).$(SOVERSION) \
	       $(NATIVE_DIR)/$(LIB_NAME).$(VERSION)
	cp -f $(LIB) $(NATIVE_DIR)/$(LIB_NAME)
	@echo "installed $(NATIVE)"

# ---- command line tool and test client -----------------------------------

tools: $(BUILD)/wsc-dump $(BUILD)/wsc-testclient

$(BUILD)/wsc-dump: tools/wsc_dump.c $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)

$(BUILD)/wsc-testclient: tests/testclient.c $(GEN_TEST_C) $(GEN_H)
	$(CC) $(ALL_CFLAGS) -o $@ tests/testclient.c $(GEN_TEST_C) $(ALL_LDFLAGS)

# ---- tests ---------------------------------------------------------------

TEST_BIN     := $(BUILD)/test_convert

unittest: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): tests/test_convert.c $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)

# Both layers, from a clean tree: build everything first, then run.
test: all unittest
	$(PY) -m pytest -q tests

# Integration tests only (assumes `make` already built the library and the
# test client).
integration:
	$(PY) -m pytest -q tests

# ---- demo ----------------------------------------------------------------

# ---- install -------------------------------------------------------------

install: $(LIB)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m755 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	cd $(DESTDIR)$(PREFIX)/lib && \
		ln -sf $(LIB_NAME).$(VERSION) $(LIB_NAME).$(SOVERSION) && \
		ln -sf $(LIB_NAME).$(VERSION) $(LIB_NAME)
	install -m644 include/wl_screencopy.h $(DESTDIR)$(PREFIX)/include/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB_NAME)* \
	      $(DESTDIR)$(PREFIX)/include/wl_screencopy.h

clean:
	rm -rf $(BUILD) $(NATIVE_DIR)
	find . -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true
