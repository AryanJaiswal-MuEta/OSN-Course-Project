CC = cc
AR = ar
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS =

CORE = build/registry.o build/lifecycle.o
HEADERS = include/model.h include/scheduler.h
RUNNER = build/worker_runner.o build/worker_files.o build/worker_platform.o
RUN_HEADERS = include/model.h include/runner.h src/worker/internal.h

.PHONY: all test clean sanitize
all: build/libscheduler.a build/scheduler_demo build/librunner.a build/worker_runner

build:
	mkdir -p build

build/%.o: src/scheduler/%.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/libscheduler.a: $(CORE)
	$(AR) rcs $@ $(CORE)

build/worker_%.o: src/worker/%.c $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/librunner.a: $(RUNNER)
	$(AR) rcs $@ $(RUNNER)

build/worker_runner: tools/worker_runner.c build/librunner.a $(RUN_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/librunner.a $(LDFLAGS) -o $@

# Fixture pe sanitizer mat lagao: small RLIMIT_AS ke andar ASan ka shadow
# mapping fit nahi hota. Runner/library/tests khud sanitize honge.
build/test_job: tests/programs/job.c | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -g $< -o $@

build/test_runner: tests/test_runner.c build/librunner.a $(RUN_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/librunner.a $(LDFLAGS) -o $@

build/scheduler_demo: tools/scheduler_demo.c build/libscheduler.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(LDFLAGS) -o $@

build/test_scheduler: tests/test_scheduler.c build/libscheduler.a $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(LDFLAGS) -o $@

test: build/test_scheduler build/scheduler_demo build/test_runner build/worker_runner build/test_job
	./build/test_scheduler
	python3 tests/test_demo.py ./build/scheduler_demo
	./build/test_runner ./build/test_job
	python3 tests/test_runner.py ./build/worker_runner ./build/test_job

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test

clean:
	rm -rf build
