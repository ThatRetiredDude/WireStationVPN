# wirestation-server: the WireGuard server as a PS5 payload.
#
#   make PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk     wirestation-server.elf
#   make host                                     build/host/wirestation-server (Linux, for tests)
#   make send PS5_HOST=192.168.1.50               sends the .elf to the ELF loader (port 9021)

root    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
lw      := $(root)/third_party/lwip/src
wgl     := $(root)/third_party/wireguard-lwip/src
VERSION ?= $(shell git -C $(root) describe --always --dirty 2>/dev/null || echo dev)

# lwIP's udp.c is left out: server/wg_udp.c stands in for it.
SRCS := $(filter-out $(lw)/core/udp.c,$(wildcard $(lw)/core/*.c)) $(wildcard $(lw)/core/ipv4/*.c) \
        $(wildcard $(wgl)/*.c) $(wildcard $(wgl)/crypto/refc/*.c) \
        $(root)/server/wg_udp.c $(root)/server/main.c
HDRS := $(wildcard $(root)/server/*.h) $(root)/server/arch/cc.h
# gnu11: wg_udp.c reads the clocks, and lwIP wants ssize_t from the system.
FLAGS := -std=gnu11 -O2 -Wall -I $(root)/server -I $(lw)/include -I $(wgl) '-DWS_VERSION="$(VERSION)"'

PS5_HOST ?= ps5
PS5_PORT ?= 9021
ELF      := wirestation-server.elf

.PHONY: all host send clean
all: $(ELF)

$(ELF): $(SRCS) $(HDRS)
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is undefined)
endif
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(FLAGS) -DWS_PS5=1 -o $@ $(SRCS)

host: $(root)/build/host/wirestation-server
$(root)/build/host/wirestation-server: $(SRCS) $(HDRS)
	mkdir -p $(dir $@)
	cc $(FLAGS) -g -pthread -o $@ $(SRCS)

send: $(ELF)
	python3 $(root)/scripts/send-elf.py $(PS5_HOST) $(PS5_PORT) $(ELF)

clean:
	rm -f $(ELF) $(root)/build/host/wirestation-server
