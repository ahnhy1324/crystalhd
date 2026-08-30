CXX ?= g++
CPPFLAGS += -D__LINUX_USER__ -I../include -I../linux_lib/libcrystalhd
LDFLAGS += -L../linux_lib/libcrystalhd
LDLIBS += -lcrystalhd -lpthread

% : %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

.PHONY: all clean
all: hellobcm mpeg2test

clean:
	rm -f hellobcm mpeg2test
