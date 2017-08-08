#!/bin/sh

# The usual bootstrap self-consistency check is three stages, but because of
# having our own libc as well we actually need four stages:
#
# host - completely different compiler
# ncc1 - ncc source compiled with host, linked against system libc
# ncc2 - ncc source compiled with ncc + system libc, linked against naive libc
# ncc3 - ncc source compiled with ncc + naive libc, linked against naive libc
# ncc4 - ncc source compiled with ncc + naive libc, linked against naive libc
#
# ncc2 and ncc3 might be different, because the libc of the compiler they were
# compiled with is different. For example, the stability of qsort can be
# different between the two, which can affect register allocation.

make clean && make -j16 && rm -rf /tmp/naive1 && cp -r /opt/naive/ /tmp/naive1 && make clean && \
	echo '\n========= Compiled stage 1 =========\n' && \
	CC=/tmp/naive1/ncc AR=/tmp/naive1/nar NAIVE_DIR=/tmp/naive1/ make -j16 && \
		rm -rf /tmp/naive2 && cp -r /opt/naive /tmp/naive2 && make clean && \
	echo '\n========= Compiled stage 2 =========\n' && \
	CC=/tmp/naive2/ncc AR=/tmp/naive2/nar NAIVE_DIR=/tmp/naive2/ make -j16 && \
		rm -rf /tmp/naive3 && cp -r /opt/naive /tmp/naive3 && make clean && \
	echo '\n========= Compiled stage 3 =========\n' && \
	CC=/tmp/naive3/ncc AR=/tmp/naive3/nar NAIVE_DIR=/tmp/naive3/ make -j16 && \
		rm -rf /tmp/naive4 && cp -r /opt/naive /tmp/naive4 && make clean && \
	echo '\n========= Compiled stage 4 =========\n' && \
	diff /tmp/naive3/ncc /tmp/naive4/ncc && \
	diff /tmp/naive3/nar /tmp/naive4/nar && \
	diff /tmp/naive3/libc.a /tmp/naive4/libc.a && \
	echo 'Bootstrap completed successfully - stage 3 and 4 are consistent'
