# msc_disk.c の BOOTWAIT をホストで試験する (TinyUSB と pico-sdk は stub/ の最小限で置き換える): make -C test/bootwait
TOP = ../..
CFLAGS = -w -DHID_KEYBOARD=1 -Istub -I$(TOP)/src

test: bootwait_test
	./bootwait_test

bootwait_test: bootwait_test.c $(TOP)/src/msc_disk.c $(TOP)/src/virtual_disk.h stub/tusb.h stub/pico/stdlib.h
	$(CC) $(CFLAGS) -o $@ bootwait_test.c

clean:
	rm -f bootwait_test

.PHONY: test clean
