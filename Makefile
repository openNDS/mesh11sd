# Host convenience build (OpenWrt uses CMake via openwrt/mesh11sd/Makefile)
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -D_GNU_SOURCE
CPPFLAGS += -Isrc/lib -DMESH11SD_VERSION=\"8.0.0_alpha1\"
SRCS = src/mesh11sd/main.c src/mesh11sd/cli.c src/mesh11sd/daemon.c \
	src/lib/ctx.c src/lib/uci_ops.c src/lib/log.c src/lib/hex.c src/lib/factory_mac.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean host-check

all: mesh11sd

mesh11sd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

host-check: clean
	$(CC) $(CFLAGS) $(CPPFLAGS) -o mesh11sd $(SRCS)
	./mesh11sd version
	./mesh11sd is_hex deadbeef
	./mesh11sd is_hex not-hex; test $$? -ne 0
	./mesh11sd str_to_hex ab | grep -qx 6162
	./mesh11sd hex_to_str 6162 | grep -qx ab
	./mesh11sd is_ipv4addr_valid 192.168.1.1 | grep -qx 192.168.1.1
	@echo host-check ok

clean:
	rm -f mesh11sd $(OBJS) src/lib/*.o src/mesh11sd/*.o
