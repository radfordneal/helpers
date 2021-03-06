# HELPERS - A LIBRARY SUPPORTING COMPUTATIONS USING HELPER THREADS
#           Makefile for Compiling Example Programs
#
# Copyright (c) 2013, 2018 Radford M. Neal.
# 
#   The helpers library is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation; either version 2 of the License, or
#   (at your option) any later version.
# 
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
# 
#   You should have received a copy of the GNU General Public License along
#   with this program; if not, write to the Free Software Foundation, Inc.,
#   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.


WARN=  # -Wall -Wno-parentheses -Wno-unused-value
COMPILE=gcc -fopenmp -g ${WARN} -O2                  # command for gcc/Intel
COMPILE_NO_OPENMP=gcc -g ${WARN} -O2

#COMPILE=cc -xopenmp -DASSUME_ATOMIC_READ_CHAR  # command for cc/SPARC
#COMPILE_NO_OPENMP=cc

all:		example example-d example-m simple simple-debug \
		merge parex parex-d

example:	example.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE} -I.. -I. example.c ../helpers.c -lm -o example

example-d:	example.c helpers-app.h ../helpers.h
	${COMPILE_NO_OPENMP} -I.. -I. -DHELPERS_DISABLED \
	example.c -lm -o example-d

example-m:	example.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE_NO_OPENMP} -I.. -I. -DHELPERS_NO_MULTITHREADING \
	example.c ../helpers.c -lm -o example-m

simple:		simple.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE} -I.. -I. -DHELPERS_NO_HOLDING simple.c ../helpers.c -lm \
	           -o simple

simple-debug:	simple.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE} -I.. -I. \
	           -DENABLE_DEBUG=1 -DENABLE_TRACE=1 -DHELPERS_NO_HOLDING \
	           simple.c ../helpers.c -lm \
	           -o simple-debug

merge:		merge.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE} -I.. -I. -DMERGE -DENABLE_TRACE=1 merge.c ../helpers.c -lm \
	           -o merge

parex:		parex.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE} -I.. -I. parex.c ../helpers.c -lm -o parex

parex-d:	parex.c helpers-app.h ../helpers.c ../helpers.h
	${COMPILE} -I.. -I. -DHELPERS_DISABLED parex.c -lm -o parex-d

clean:
	rm -f example example-d example-m simple simple-debug \
	      merge parex parex-d
