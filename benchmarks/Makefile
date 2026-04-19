# makefile for cxl-hammer benchmarks

CC = gcc
CFLAGS = -O2 -static -Wall
M5_PATH = ../util/m5
GEM5_INCLUDE = ../include

M5_INCLUDE = -I$(GEM5_INCLUDE)
M5_LIB = $(M5_PATH)/build/x86/out/libm5.a

TARGETS = rowhammer_cxl_test

all: $(TARGETS)

rowhammer_cxl_test: rowhammer_cxl_test.c
	@if [ ! -f "$(M5_LIB)" ]; then \
		echo "Building m5ops library..."; \
		cd $(M5_PATH) && scons build/x86/out/libm5.a; \
	fi
	$(CC) $(CFLAGS) $(M5_INCLUDE) -o $@ $< $(M5_LIB)
	@echo "Built: $@ (with m5ops linked)"

clean:
	rm -f $(TARGETS) *.o

install: $(TARGETS)
	@if [ -d "/tmp/parsec_mount/home/cxl_benchmark" ]; then \
		echo "Installing to disk image..."; \
		sudo cp rowhammer_cxl_test /tmp/parsec_mount/home/cxl_benchmark/rowhammer_cxl_test; \
		sudo chmod +x /tmp/parsec_mount/home/cxl_benchmark/rowhammer_cxl_test; \
		echo "Installed: /home/cxl_benchmark/rowhammer_cxl_test"; \
	else \
		echo "ERROR: Disk image not mounted at /tmp/parsec_mount"; \
		echo "Run: sudo mount -o loop,offset=\$$((2048*512)) fs_files/parsec.img /tmp/parsec_mount"; \
	fi

.PHONY: all clean install
