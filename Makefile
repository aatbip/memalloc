CC = gcc
CC_FLAGS = -Wall -O2 -o

memalloc: memalloc.c memalloc.h
	${CC} $< ${CC_FLAGS} $@

run: memalloc
	@echo "======================" 
	./$^

memalloc_tsan: memalloc.c memalloc.h
	${CC} -O1 -g -fsanitize=thread $< -o $@ -pthread

run_tsan: memalloc_tsan
	@echo "======================" 
	./$^
