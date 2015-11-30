CXX 	= gcc -g

INSTALL_DIR = /usr/local

FORKGC = libforkgc.so
TARGETS	= $(FORKGC)

FORKGC_SRC = queue.c env.c wrappers.c alloc.c util.c thread.c	\
	proc.c forkgc.c child.c threadscan.c
FORKGC_OBJ = $(FORKGC_SRC:.c=.o)

# The -fno-zero-initialized-in-bss flag appears to be busted.
#CFLAGS = -fno-zero-initialized-in-bss
CFLAGS := -O2
ifndef DEBUG
	CFLAGS := $(CFLAGS) -DNDEBUG
endif
LDFLAGS = -ldl -pthread

all:	$(TARGETS)

debug:
	DEBUG=1 make all

$(FORKGC): $(FORKGC_OBJ)
	$(CXX) $(CFLAGS) -shared -Wl,-soname,$@ -o $@ $^ $(LDFLAGS)

$(INSTALL_DIR)/lib/$(FORKGC): $(FORKGC)
	cp $< $@

$(INSTALL_DIR)/include/forkgc.h: include/forkgc.h
	cp $< $@

install: $(INSTALL_DIR)/lib/$(FORKGC) $(INSTALL_DIR)/include/forkgc.h
	ldconfig

clean:
	rm -f *.o $(TARGETS) core

%.o: %.c
	$(CXX) $(CFLAGS) -o $@ -Wall -fPIC -c -ldl $<

