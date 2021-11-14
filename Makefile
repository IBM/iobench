CC = gcc
CFLAGS = -g -O2 -Wall -Werror -D_USE_DEBUG_PFX_ -pthread
LDFLAGS = -pthread

all: iobench

logger.o: logger.c logger.h Makefile
iobench.o: iobench.c iobench.h logger.h compiler.h Makefile
iobench_parse_args.o: iobench_parse_args.c iobench.h logger.h Makefile

iobench: iobench.o iobench_parse_args.o logger.o

clean:
	rm -f iobench *.o
