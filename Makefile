# Build the kernel module (mychardev.ko) and the user-space app (test_app).
#   make            build both
#   sudo make load  insmod the module
#   make demo       run the demo      make test   run the automated tests
#   sudo make unload / make clean
obj-m += mychardev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: module test_app

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test_app: test_app.c mychardev.h
	$(CC) -Wall -Wextra -O2 -pthread -o $@ test_app.c

load: module
	insmod mychardev.ko
	dmesg | tail -n 3

unload:
	rmmod mychardev
	dmesg | tail -n 3

demo: test_app
	./test_app

test: test_app
	./test_app --test

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_app

.PHONY: all module load unload demo test clean
