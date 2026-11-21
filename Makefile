CC = cc
AR = ar
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS =

CORE = build/registry.o build/lifecycle.o build/loop.o build/service.o
HEADERS = include/model.h include/scheduler.h include/scheduler_loop.h include/scheduler_service.h
COMMON = build/protocol.o build/transport.o build/discovery.o
COMMON_HEADERS = include/protocol.h include/transport.h include/discovery.h
RUNNER = build/worker_runner.o build/worker_files.o build/worker_platform.o
RUN_HEADERS = include/model.h include/runner.h include/worker_config.h include/worker_loop.h include/worker_discovery.h include/discovery.h src/worker/internal.h

.PHONY: all test clean sanitize
all: build/libscheduler.a build/librunner.a build/scheduler build/worker build/client

build:
	mkdir -p build

build/protocol.o: src/common/protocol.c include/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/transport.o: src/common/transport.c include/transport.h include/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/discovery.o: src/common/discovery.c include/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/time_log.o: src/common/time_log.c include/time_log.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/%.o: src/scheduler/%.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/libscheduler.a: $(CORE)
	$(AR) rcs $@ $(CORE)

build/worker_%.o: src/worker/%.c $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/librunner.a: $(RUNNER)
	$(AR) rcs $@ $(RUNNER)

build/scheduler: src/scheduler/main.c build/libscheduler.a $(COMMON) $(HEADERS) $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(COMMON) $(LDFLAGS) -o $@

build/worker: src/worker/main.c build/worker_config.o build/worker_loop.o build/worker_discovery.o build/librunner.a $(COMMON) $(RUN_HEADERS) $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_config.o build/worker_loop.o build/worker_discovery.o build/librunner.a $(COMMON) $(LDFLAGS) -o $@

.PHONY: test-worker-discovery
test-worker-discovery: build/test_worker_discovery build/worker
	python3 tests/test_worker_discovery.py ./build/test_worker_discovery ./build/worker

build/test_worker_discovery: tests/test_worker_discovery.c build/worker_discovery.o build/discovery.o $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_discovery.o build/discovery.o $(LDFLAGS) -o $@

build/client: src/client/main.c $(COMMON) $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(COMMON) $(LDFLAGS) -o $@



build/test_protocol: tests/test_protocol.c src/common/protocol.c include/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/common/protocol.c $(LDFLAGS) -o $@

build/test_discovery: tests/test_discovery.c $(COMMON) $(COMMON_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(COMMON) $(LDFLAGS) -o $@

build/test_network: tests/test_network.c $(COMMON) $(COMMON_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(COMMON) $(LDFLAGS) -o $@

build/test_policy: tests/test_policy.c build/libscheduler.a $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(LDFLAGS) -o $@

build/test_integration: tests/test_integration.c build/libscheduler.a build/librunner.a build/worker_config.o build/worker_loop.o $(COMMON) $(HEADERS) $(RUN_HEADERS) $(COMMON_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a build/librunner.a build/worker_config.o build/worker_loop.o $(COMMON) $(LDFLAGS) -o $@

test: build/test_protocol build/test_discovery build/test_network build/test_policy build/test_integration
	./build/test_protocol
	./build/test_discovery
	./build/test_network
	./build/test_policy
	./build/test_integration

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test

clean:
	rm -rf build
