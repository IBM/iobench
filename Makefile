CC = gcc
CFLAGS = -g -O2 -Wall -Werror -D_USE_DEBUG_PFX_ -pthread
LDFLAGS = -pthread -lrt

all: iobench

logger.o: logger.c logger.h Makefile

iobench.o: iobench.c iobench.h logger.h compiler.h Makefile
iobench_parse_args.o: iobench_parse_args.c iobench.h logger.h Makefile
dio_posix.o: dio_posix.c iobench.h logger.h Makefile
iobench: iobench.o iobench_parse_args.o dio_posix.o logger.o
	$(CC) -o $@ iobench.o iobench_parse_args.o dio_posix.o logger.o ${LDFLAGS}

clean:
	rm -f iobench *.o
