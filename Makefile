CC = gcc
CFLAGS = -g -O2 -Wall -Werror -D_USE_DEBUG_PFX_ -pthread
LDFLAGS = -pthread -lrt -laio

all: iobench

logger.o: logger.c logger.h Makefile

iobench.o: iobench.c iobench.h logger.h compiler.h Makefile
iobench_parse_args.o: iobench_parse_args.c iobench.h logger.h Makefile
aio_posix.o: aio_posix.c iobench.h logger.h compiler.h Makefile
aio_linux.o: aio_linux.c iobench.h logger.h compiler.h Makefile
dio.o: dio.c iobench.h logger.h compiler.h Makefile
core_affinity.o: core_affinity.c  logger.h Makefile
iobench: iobench.o iobench_parse_args.o aio_posix.o aio_linux.o dio.o core_affinity.o logger.o
	$(CC) -o $@ iobench.o iobench_parse_args.o aio_posix.o aio_linux.o dio.o core_affinity.o logger.o ${LDFLAGS}

clean:
	rm -f iobench *.o
