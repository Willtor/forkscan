CXX 	= gcc -g

INSTALL_DIR = /usr/local

FORKSCAN = libforkscan.so
TARGETS	= $(FORKSCAN)

FORKSCAN_SRC =	\
	queue.c	\
	env.c	\
	wrappers.c	\
	alloc.c	\
	util.c	\
	buffer.c	\
	thread.c	\
	proc.c	\
	forkgc.c	\
	child.c	\
	threadscan.c

FORKSCAN_OBJ = $(FORKSCAN_SRC:.c=.o)

# The -fno-zero-initialized-in-bss flag appears to be busted.
#CFLAGS = -fno-zero-initialized-in-bss
CFLAGS := -O2 -DJEMALLOC_NO_DEMANGLE
#CFLAGS += -DTIMING
ifndef DEBUG
	CFLAGS := $(CFLAGS) -DNDEBUG
endif
LDFLAGS = -ldl -pthread

all:	$(TARGETS)

debug:
	DEBUG=1 make all

$(FORKSCAN): $(FORKSCAN_OBJ)
	$(CXX) $(CFLAGS) -shared -Wl,-soname,$@ -o $@ $^ $(LDFLAGS)

$(INSTALL_DIR)/lib/$(FORKSCAN): $(FORKSCAN)
	cp $< $@

$(INSTALL_DIR)/include/forkscan.h: include/forkscan.h
	cp $< $@

install: $(INSTALL_DIR)/lib/$(FORKSCAN) $(INSTALL_DIR)/include/forkscan.h
	ldconfig

clean:
	rm -f *.o $(TARGETS) core

%.o: %.c
	$(CXX) $(CFLAGS) -o $@ -Wall -fPIC -c -ldl $<

