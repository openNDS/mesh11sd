CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -D_GNU_SOURCE
CPPFLAGS += -Isrc/lib -DMESH11SD_VERSION=\"8.0.0_alpha24\"
SRCS = src/mesh11sd/main.c src/mesh11sd/cli.c src/mesh11sd/daemon.c \
	src/lib/config.c src/lib/uci_ops.c src/lib/util.c src/lib/log.c \
	src/lib/hex.c src/lib/factory_mac.c src/lib/wireless.c src/lib/auto_config.c \
	src/lib/portal.c src/lib/bridge.c src/lib/portal_metrics.c src/lib/mobility.c \
	src/lib/remote_ops.c src/lib/json_fmt.c src/lib/commit_ops.c \
	src/lib/status.c src/lib/led.c

.PHONY: all clean host-check
all: mesh11sd
mesh11sd: $(SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS)
host-check: clean
	$(CC) $(CFLAGS) $(CPPFLAGS) -o mesh11sd $(SRCS)
	./mesh11sd version
	@echo host-check ok
clean:
	rm -f mesh11sd
