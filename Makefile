CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -D_GNU_SOURCE
CPPFLAGS += -Isrc/lib -DMESH11SD_VERSION=\"8.0.0_alpha1\"
SRCS = src/mesh11sd/main.c src/mesh11sd/cli.c src/mesh11sd/daemon.c \
	src/lib/config.c src/lib/uci_ops.c src/lib/util.c src/lib/log.c \
	src/lib/hex.c src/lib/factory_mac.c src/lib/wireless.c src/lib/auto_config.c

.PHONY: all clean host-check

all: mesh11sd

mesh11sd: $(SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

host-check: clean
	$(CC) $(CFLAGS) $(CPPFLAGS) -o mesh11sd $(SRCS)
	./mesh11sd version
	./mesh11sd is_hex deadbeef
	./mesh11sd str_to_hex ab | grep -qx 6162
	@echo host-check ok

clean:
	rm -f mesh11sd src/lib/*.o src/mesh11sd/*.o
