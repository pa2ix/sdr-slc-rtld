# SDR-SLC RTL-SDR Daemon — Makefile
# Requires: librtlsdr-dev, gcc >= 7 (C11 + stdatomic)
#
# Targets:
#   make          — build the daemon
#   make debug    — build with -g -DDEBUG -fsanitize=address
#   make clean    — remove build artefacts
#   make install  — install to /usr/local/bin

CC      = gcc
TARGET  = sdr-slc-rtld
PREFIX  = /usr/local

# ---- Source files ----
SRCS =  main.c                  \
        disc/disc.c             \
        cp/cp_server.c          \
        cp/cp_session.c         \
        cp/cp_commands.c        \
        vita/vita_tx.c          \
        rtl/rtl_bridge.c        \
        json/jsmn.c             \
        json/json_builder.c

OBJS = $(SRCS:.c=.o)

# ---- Flags ----
CFLAGS_COMMON = \
    -std=c11            \
    -Wall               \
    -Wextra             \
    -Wpedantic          \
    -Wstrict-prototypes \
    -Wmissing-prototypes\
    -Wshadow            \
    -Wno-unused-parameter \
    -D_GNU_SOURCE       \
    -pthread

CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -DNDEBUG
CFLAGS_DEBUG   = $(CFLAGS_COMMON) -g3 -O0 -DDEBUG \
                 -fsanitize=address,undefined \
                 -fno-omit-frame-pointer

CFLAGS ?= $(CFLAGS_RELEASE)

LDFLAGS = -lrtlsdr -lpthread -lm

# ---- Rules ----
.PHONY: all debug clean install uninstall

all: $(TARGET)

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built: $@"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Header dependencies (regenerate with: gcc -MM *.c */*.c)
main.o: main.c config.h disc/disc.h cp/cp_server.h rtl/rtl_bridge.h vita/vita_tx.h

disc/disc.o: disc/disc.c disc/disc.h config.h

cp/cp_server.o: cp/cp_server.c cp/cp_server.h cp/cp_session.h config.h

cp/cp_session.o: cp/cp_session.c cp/cp_session.h cp/cp_commands.h \
    json/jsmn.h json/json_builder.h config.h vita/vita_tx.h

cp/cp_commands.o: cp/cp_commands.c cp/cp_commands.h cp/cp_session.h \
    json/json_builder.h json/jsmn.h rtl/rtl_bridge.h vita/vita_tx.h config.h

vita/vita_tx.o: vita/vita_tx.c vita/vita_tx.h rtl/rtl_bridge.h config.h

rtl/rtl_bridge.o: rtl/rtl_bridge.c rtl/rtl_bridge.h config.h

json/jsmn.o: json/jsmn.c json/jsmn.h

json/json_builder.o: json/json_builder.c json/json_builder.h json/jsmn.h

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(PREFIX)/bin/$(TARGET)
	@echo "Installed to $(PREFIX)/bin/$(TARGET)"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

# ---- Optional: systemd service file ----
install-service: install
	@echo "[Unit]"                                              > /tmp/sdr-slc-rtld.service
	@echo "Description=SDR-SLC RTL-SDR Daemon"               >> /tmp/sdr-slc-rtld.service
	@echo "After=network.target"                              >> /tmp/sdr-slc-rtld.service
	@echo ""                                                  >> /tmp/sdr-slc-rtld.service
	@echo "[Service]"                                         >> /tmp/sdr-slc-rtld.service
	@echo "Type=simple"                                       >> /tmp/sdr-slc-rtld.service
	@echo "ExecStart=$(PREFIX)/bin/$(TARGET) -i eth0"        >> /tmp/sdr-slc-rtld.service
	@echo "Restart=on-failure"                                >> /tmp/sdr-slc-rtld.service
	@echo "RestartSec=5"                                      >> /tmp/sdr-slc-rtld.service
	@echo ""                                                  >> /tmp/sdr-slc-rtld.service
	@echo "[Install]"                                         >> /tmp/sdr-slc-rtld.service
	@echo "WantedBy=multi-user.target"                        >> /tmp/sdr-slc-rtld.service
	install -D -m 644 /tmp/sdr-slc-rtld.service \
	    /etc/systemd/system/sdr-slc-rtld.service
	systemctl daemon-reload
	@echo "Service installed. Enable with: systemctl enable --now sdr-slc-rtld"
