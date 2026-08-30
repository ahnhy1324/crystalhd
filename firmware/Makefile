FIRMWARE  = fwbin/70015/bcm70015fw.bin \
            fwbin/70012/bcm70012fw.bin
FIRMWAREDIR ?= /lib/firmware
DESTDIR ?=

.PHONY: install
install:
	install -d $(DESTDIR)$(FIRMWAREDIR)
	install -m 0644 $(FIRMWARE) $(DESTDIR)$(FIRMWAREDIR)
