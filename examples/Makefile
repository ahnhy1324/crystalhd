CXX ?= g++
CPPFLAGS += -D__LINUX_USER__ -I../include -I../linux_lib/libcrystalhd
LDFLAGS += -L../linux_lib/libcrystalhd
LDLIBS += -lcrystalhd -lpthread

% : %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

.PHONY: all clean
all: hellobcm mpeg2test

clean:
	rm -f hellobcm mpeg2test $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_CPU_STAMP).tmp

include ../cpu.mk
hellobcm mpeg2test: $(CRYSTALHD_BUILD_DEPS) ../linux_lib/libcrystalhd/libcrystalhd.so
