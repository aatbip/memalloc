CC = gcc
CC_FLAGS = -Wall -O2 -o

memalloc: memalloc.c memalloc.h
	${CC} $< ${CC_FLAGS} $@

run: memalloc
	@echo "======================" 
	./$^

CC_TSAN_FLAGS = -O1 -g -fsanitize=thread,undefined -pthread -o
memalloc_tsan: memalloc.c memalloc.h
	${CC} $< ${CC_TSAN_FLAGS} $@

run_tsan: memalloc_tsan
	@echo "======================" 
	./$^

CC_ASAN_FLAGS = -O1 -g -fsanitize=address,undefined -o
memalloc_asan: memalloc.c memalloc.h
	${CC} $< ${CC_ASAN_FLAGS} $@

run_asan: memalloc_asan
	@echo "======================" 
	./$^

CC_DEBUG_FLAGS = -g -O0 -o
memalloc_gdb: memalloc.c memalloc.h
	${CC} $< ${CC_DEBUG_FLAGS} $@

run_gdb: memalloc_gdb
	@echo "======================" 
	gdb ./$^

