CC = gcc
CC_FLAGS = -Wall -O2 -o

memalloc: memalloc.c memalloc.h
	${CC} $< ${CC_FLAGS} $@

run: memalloc
	@echo "======================" 
	./$^
