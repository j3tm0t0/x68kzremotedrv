# msc_disk.c の BOOTWAIT をホストで試験する (TinyUSB と pico-sdk は stub/ の最小限で置き換える): make -C test/bootwait
TOP = ../..
CFLAGS = -w -DHID_KEYBOARD=1 -Istub -I$(TOP)/src

test: bootwait_test waiter
	./bootwait_test

# 待ち IPL (driver/bootwait*.S) を Unicorn の 68000 で、Pico 側 (src/bootwait.c) と組み合わせて試す。
# m68k-xelf-gcc と python3 の unicorn が要る
waiter: libbw.dylib
	python3 waiter_test.py

libbw.dylib: $(TOP)/src/bootwait.c $(TOP)/src/bootwait.h $(TOP)/src/bootwait_proto.h fakeflash.c ../../driver/bootwait.inc ../../driver/bootwait1.inc
	$(CC) -shared -O2 -Wall -I$(TOP)/src -I$(TOP)/driver -o $@ $(TOP)/src/bootwait.c fakeflash.c

../../driver/bootwait.inc ../../driver/bootwait1.inc: ../../driver/bootwait.S ../../driver/bootwait1.S
	$(MAKE) -C ../../driver bootwait.inc bootwait1.inc

bootwait_test: bootwait_test.c $(TOP)/src/msc_disk.c $(TOP)/src/virtual_disk.h stub/tusb.h stub/pico/stdlib.h
	$(CC) $(CFLAGS) -o $@ bootwait_test.c

clean:
	rm -f bootwait_test libbw.dylib

.PHONY: test clean
