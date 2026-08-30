# Broadcom BCM970012/BCM970015 Crystal HD userspace interface library.

BCLIB_MAJOR := 3
BCLIB_ABI_MINOR := 6
BCLIB_NAME := libcrystalhd.so
BCLIB_SONAME := $(BCLIB_NAME).$(BCLIB_MAJOR)
BCLIB := $(BCLIB_NAME).$(BCLIB_MAJOR).$(BCLIB_ABI_MINOR)

CXX ?= g++
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
FIRMWAREDIR ?= /lib/firmware
DESTDIR ?=

ROOTDIR := ../..
CPPFLAGS += -D__LINUX_USER__ -I. -I$(ROOTDIR)/include -I$(ROOTDIR)/include/link
CXXFLAGS ?= -O2
CXXFLAGS += -Wall -fPIC -fstrict-aliasing -msse2
LDFLAGS += -shared -Wl,-soname,$(BCLIB_SONAME)
LDLIBS += -pthread

SRCFILES := libcrystalhd_if.cpp \
	libcrystalhd_int_if.cpp \
	libcrystalhd_fwcmds.cpp \
	libcrystalhd_priv.cpp \
	libcrystalhd_fwdiag_if.cpp \
	libcrystalhd_fwload_if.cpp \
	libcrystalhd_parser.cpp
OBJFILES := $(SRCFILES:.cpp=.o)
PUBLIC_HEADERS := libcrystalhd_if.h \
	$(ROOTDIR)/include/bc_dts_defs.h \
	$(ROOTDIR)/include/bc_dts_types.h \
	$(ROOTDIR)/include/libcrystalhd_version.h
FIRMWARE := $(ROOTDIR)/firmware/fwbin/70012/bcm70012fw.bin \
	$(ROOTDIR)/firmware/fwbin/70015/bcm70015fw.bin

.PHONY: all clean install

all: $(BCLIB) $(BCLIB_NAME) $(BCLIB_SONAME) libcrystalhd.pc

$(BCLIB): $(OBJFILES)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BCLIB_NAME) $(BCLIB_SONAME): $(BCLIB)
	ln -sfn $(BCLIB) $@

libcrystalhd.pc: libcrystalhd.pc.in
	sed -e 's|@prefix@|$(PREFIX)|g' \
		-e 's|@libdir@|$(LIBDIR)|g' \
		-e 's|@includedir@|$(INCLUDEDIR)|g' $< > $@

clean:
	rm -f $(BCLIB) $(BCLIB_SONAME) $(BCLIB_NAME) $(OBJFILES) libcrystalhd.pc

install: all
	install -D -m 0755 $(BCLIB) $(DESTDIR)$(LIBDIR)/$(BCLIB)
	ln -sfn $(BCLIB) $(DESTDIR)$(LIBDIR)/$(BCLIB_SONAME)
	ln -sfn $(BCLIB_SONAME) $(DESTDIR)$(LIBDIR)/$(BCLIB_NAME)
	install -d $(DESTDIR)$(INCLUDEDIR)/libcrystalhd
	install -m 0644 $(PUBLIC_HEADERS) $(DESTDIR)$(INCLUDEDIR)/libcrystalhd
	install -D -m 0644 libcrystalhd.pc $(DESTDIR)$(PKGCONFIGDIR)/libcrystalhd.pc
	install -d $(DESTDIR)$(FIRMWAREDIR)
	install -m 0644 $(FIRMWARE) $(DESTDIR)$(FIRMWAREDIR)
