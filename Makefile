CC = gcc
CFLAGS = -g -O2 -Wall -Wno-stringop-overread -Werror -D_USE_DEBUG_PFX_ -pthread
LDFLAGS = -pthread -lrt -laio

all: iobench

logger.o: logger.c logger.h Makefile

iobench.o: iobench.c iobench.h logger.h compiler.h core_affinity.h Makefile
iobench_parse_args.o: iobench_parse_args.c iobench.h logger.h Makefile
aio_posix.o: aio_posix.c iobench.h logger.h compiler.h Makefile
aio_block.o: aio_block.c iobench.h logger.h compiler.h Makefile
aio_linux.o: aio_linux.c iobench.h logger.h Makefile
dio.o: dio.c iobench.h logger.h compiler.h Makefile
core_affinity.o: core_affinity.c  logger.h core_affinity.h Makefile
uring.o: uring.c uring.h logger.h compiler.h Makefile
scsi.o: scsi.c scsi.h logger.h compiler.h Makefile
aio_scsi.o: aio_scsi.c scsi.h iobench.h logger.h compiler.h Makefile

iobench: iobench.o iobench_parse_args.o aio_posix.o aio_block.o dio.o core_affinity.o uring.o aio_linux.o aio_scsi.o scsi.o logger.o
	$(CC) -o $@ iobench.o iobench_parse_args.o aio_posix.o aio_block.o dio.o core_affinity.o aio_linux.o uring.o aio_scsi.o scsi.o logger.o ${LDFLAGS}

clean:
	rm -f iobench *.o
