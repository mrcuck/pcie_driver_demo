# Makefile for mydma driver and test program
.PHONY: all modules test_app clean

# Build both the kernel module and test app by default
all: modules test_app

# --- Kernel Module Targets ---
obj-m += mydma.o

modules:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# --- Userspace Test App Targets ---
test_app: test_dma.c
	$(CC) test_dma.c -o test_dma -Wall

# --- Cleanup Targets ---
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f test_dma