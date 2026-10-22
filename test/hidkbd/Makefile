# hidkbd_cmd.c (UDP 6869 のコマンド -> HID レポート) をホストで試験する: make -C test/hidkbd
TOP = ../..
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2 -I. -I$(TOP)/src -I$(TOP)/pico-sdk/lib/tinyusb/src

test: hidkbd_test
	./hidkbd_test

hidkbd_test: hidkbd_test.c $(TOP)/src/hidkbd_cmd.c $(TOP)/src/hidkbd_cmd.h
	$(CC) $(CFLAGS) -o $@ hidkbd_test.c $(TOP)/src/hidkbd_cmd.c

clean:
	rm -f hidkbd_test

.PHONY: test clean
