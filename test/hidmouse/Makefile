# hidmouse_cmd.c (UDP 6869 の "mouse ..." -> HID マウスレポート) をホストで試験する: make -C test/hidmouse
TOP = ../..
CFLAGS = -Wall -Wextra -Wno-unused-parameter -O2 -I$(TOP)/src

test: hidmouse_test
	./hidmouse_test

hidmouse_test: hidmouse_test.c $(TOP)/src/hidmouse_cmd.c $(TOP)/src/hidmouse_cmd.h
	$(CC) $(CFLAGS) -o $@ hidmouse_test.c $(TOP)/src/hidmouse_cmd.c

clean:
	rm -f hidmouse_test

.PHONY: test clean
