#
# Makefile for the `orthus` example use case.
#


OCFDIR=../../
SRCDIR=src/
INCDIR=include/

SRC=$(shell find ${SRCDIR} -name \*.c)
OBJS=$(patsubst %.c, %.o, $(SRC))
PROGRAM=bench

CC=gcc
CFLAGS=-Wall -I${INCDIR} -I${SRCDIR} -I${SRCDIR}/ocf/env/ -O2 -Wno-stringop-truncation
LDFLAGS=-lm -lz -pthread


all: sync
	$(MAKE) $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

sync:
	@$(MAKE) -C ${OCFDIR} inc O=$(PWD)
	@$(MAKE) -C ${OCFDIR} src O=$(PWD)
	@$(MAKE) -C ${OCFDIR} env O=$(PWD) OCF_ENV=posix

clean:
	@rm -rf $(PROGRAM) $(OBJS)

libclean:
	@rm -rf $(PROGRAM) $(OBJS)
	@rm -rf src/ocf
	@rm -rf include/ocf

.PHONY: all sync clean libclean
