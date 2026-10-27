CC = cc
AR = ar
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS =

CORE = build/registry.o build/lifecycle.o
HEADERS = include/model.h include/scheduler.h

.PHONY: all test clean sanitize
all: build/libscheduler.a build/scheduler_demo

build:
	mkdir -p build

build/%.o: src/scheduler/%.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/libscheduler.a: $(CORE)
	$(AR) rcs $@ $(CORE)

build/scheduler_demo: tools/scheduler_demo.c build/libscheduler.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(LDFLAGS) -o $@

build/test_scheduler: tests/test_scheduler.c build/libscheduler.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(LDFLAGS) -o $@

test: build/test_scheduler build/scheduler_demo
	./build/test_scheduler
	python3 tests/test_demo.py ./build/scheduler_demo

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test

clean:
	rm -rf build
