# SPDX-License-Identifier: MIT
# sdr-slc-rtld — example / reference code. Do as you like; see LICENSE. Ivo van Ling (PA2IX).

# sdr-slc-rtld - SDR-SLC bridge for an RTL-SDR dongle
#
# Same shape as the sdr-slc-bladerfd Makefile so the two daemons build,
# install and test the same way.
#
#   make            production build (needs librtlsdr-dev; see MDNS= below)
#   make test-stub  no-dongle build against test/rtlsdr_stub.c
#   make check      build the stub and run every suite in test/
#   make install    binary, systemd unit, /etc/sdr-slc/rtld.conf (kept if present)

BIN      := sdr-slc-rtld
CONFDIR  := /etc/sdr-slc
PREFIX   ?= /usr/local

# ---- mDNS backend ---------------------------------------------------------
#   make                 builtin responder only; no Avahi at build or run time
#   make MDNS=avahi      builtin + Avahi backend; --mdns avahi selects it at
#                        runtime, needs libavahi-client-dev and avahi-daemon
MDNS     ?= builtin

SRC      := main.c                  \
            cp/cp_server.c          \
            cp/cp_session.c         \
            cp/cp_commands.c        \
            vita/vita_tx.c          \
            rtl/rtl_bridge.c        \
            json/jsmn.c             \
            json/json_builder.c     \
            common/auth.c           \
            common/log.c            \
            common/mdns.c           \
            common/mdns_builtin.c

CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith \
            -Wno-unused-parameter -D_GNU_SOURCE -MMD -MP -pthread -I.
MDNS_LIBS :=
ifeq ($(MDNS),avahi)
  SRC      += common/mdns_avahi.c
  CFLAGS   += -DSLC_MDNS_AVAHI $(shell pkg-config --cflags avahi-client)
  MDNS_LIBS := $(shell pkg-config --libs avahi-client)
else ifneq ($(MDNS),builtin)
  $(error MDNS must be builtin or avahi)
endif

OBJ      := $(SRC:.c=.o)
DEP      := $(OBJ:.o=.d)
LDLIBS   += $(MDNS_LIBS) -lrtlsdr -lpthread -lm

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "Built: $@"

# ---- No-dongle build for the test suites ----------------------------------
# Links test/rtlsdr_stub.c instead of librtlsdr, with -Itest/stubinc
# supplying a minimal rtl-sdr.h, so the control plane and the VITA sender
# can be exercised end to end on a machine with neither a dongle nor
# librtlsdr-dev.  Honours MDNS= like the production build.
STUB_SRC := $(SRC) test/rtlsdr_stub.c
STUB_OBJ := $(patsubst %.c,%.stub.o,$(STUB_SRC))

%.stub.o: %.c
	$(CC) $(CFLAGS) -O1 -Itest/stubinc -c $< -o $@

test-stub: $(BIN)-stub

$(BIN)-stub: $(STUB_OBJ)
	$(CC) $(STUB_OBJ) -o $@ $(LDFLAGS) $(MDNS_LIBS) -lpthread -lm
	@echo "Built: $@ (fake dongle)"

# ---- Conformance suites ---------------------------------------------------
check: $(BIN)-stub
	python3 test/test_slc_cp_r13.py   ./$(BIN)-stub
	python3 test/test_auth.py         ./$(BIN)-stub
	python3 test/test_vita_context.py ./$(BIN)-stub
	python3 test/test_mdns.py         ./$(BIN)-stub

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 systemd/sdr-slc-rtld.service \
	    $(DESTDIR)/lib/systemd/system/sdr-slc-rtld.service
	install -d -m755 $(DESTDIR)$(CONFDIR)
	@if [ -e $(DESTDIR)$(CONFDIR)/rtld.conf ]; then \
	    echo "keeping existing $(CONFDIR)/rtld.conf"; \
	else install -m644 etc/sdr-slc/rtld.conf $(DESTDIR)$(CONFDIR)/rtld.conf; fi
	install -d -m755 $(DESTDIR)$(PREFIX)/share/doc/$(BIN)
	install -m644 README.md CHANGES.md \
	    docs/capabilities-rtl.json docs/status-rtl.json \
	    $(DESTDIR)$(PREFIX)/share/doc/$(BIN)/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) \
	      $(DESTDIR)/lib/systemd/system/sdr-slc-rtld.service

clean:
	rm -f $(OBJ) $(DEP) $(STUB_OBJ) $(STUB_OBJ:.o=.d) $(BIN) $(BIN)-stub \
	      common/mdns_avahi.o common/mdns_avahi.d \
	      common/mdns_avahi.stub.o common/mdns_avahi.stub.d

-include $(DEP) $(STUB_OBJ:.o=.d)

.PHONY: all test-stub check install uninstall clean
