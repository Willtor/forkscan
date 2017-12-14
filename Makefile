CXX 	= gcc -g

INSTALL_DIR = /usr/local

MALLOCAR = supermalloc.a
FORKSCAN = libforkscan.so
TARGETS	= $(FORKSCAN)

FORKSCAN_SRC =		\
	queue.c		\
	env.c		\
	wrappers.c	\
	alloc.c		\
	util.c		\
	buffer.c	\
	thread.c	\
	proc.c		\
	forkscan.c	\
	child.c		\
	frontend.c

FORKSCAN_OBJ = $(FORKSCAN_SRC:.c=.o)

# The -fno-zero-initialized-in-bss flag appears to be busted.
#CFLAGS = -fno-zero-initialized-in-bss
CFLAGS := -O3
#CFLAGS += -DTIMING
ifndef DEBUG
	CFLAGS := $(CFLAGS) -DNDEBUG
endif
LINKMALLOC = -L. -l:$(MALLOCAR)
LDFLAGS = -ldl -pthread -Wl,-z,defs

all:	$(TARGETS)

debug:
	DEBUG=1 make all

$(FORKSCAN): $(FORKSCAN_OBJ) | $(MALLOCAR)
	$(CXX) $(CFLAGS) -shared -Wl,-soname,$@ -o $@ $^ $(LINKMALLOC) $(LDFLAGS)

$(INSTALL_DIR)/lib/$(FORKSCAN): $(FORKSCAN)
	cp $< $@

$(INSTALL_DIR)/include/forkscan.h: include/forkscan.h
	cp $< $@

$(INSTALL_DIR)/lib/def:
	mkdir -p $@

$(INSTALL_DIR)/lib/def/forkscan.defi: include/forkscan.defi $(INSTALL_DIR)/lib/def
	cp $< $@

install: $(INSTALL_DIR)/lib/$(FORKSCAN) $(INSTALL_DIR)/include/forkscan.h $(INSTALL_DIR)/lib/def/forkscan.defi
	ldconfig

clean:
	rm -f *.o $(TARGETS) core

%.o: %.c
	$(CXX) $(CFLAGS) -o $@ -Wall -fPIC -c -ldl $<

