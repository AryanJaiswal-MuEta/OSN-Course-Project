CC = cc
AR = ar
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDFLAGS =
WORKLOAD_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g

CORE = build/registry.o build/lifecycle.o build/loop.o build/service.o build/dispatch.o build/policy.o build/dependencies.o build/quotas.o build/placement.o build/preemption.o build/recovery.o build/output.o
HEADERS = include/model.h include/scheduler.h include/scheduler_loop.h include/scheduler_service.h
COMMON = build/protocol.o build/transport.o build/discovery.o build/logging.o
COMMON_HEADERS = include/protocol.h include/transport.h include/discovery.h include/logging.h
RUNNER = build/worker_runner.o build/worker_files.o build/worker_platform.o
RUN_HEADERS = include/model.h include/runner.h include/worker_config.h include/worker_loop.h include/worker_discovery.h include/worker_cache.h include/discovery.h src/worker/internal.h src/worker/node.h

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

build/logging.o: src/common/logging.c include/logging.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/time_log.o: src/common/time_log.c include/time_log.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/%.o: src/scheduler/%.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/libscheduler.a: $(CORE) Makefile
	rm -f $@
	$(AR) rcs $@ $(CORE)

build/worker_%.o: src/worker/%.c $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/librunner.a: $(RUNNER) Makefile
	rm -f $@
	$(AR) rcs $@ $(RUNNER)

build/scheduler: src/scheduler/main.c src/scheduler/network.c src/scheduler/server.h build/libscheduler.a $(COMMON) $(HEADERS) $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/scheduler/main.c src/scheduler/network.c build/libscheduler.a $(COMMON) $(LDFLAGS) -o $@

build/test_scheduler build/test_service build/test_loop build/test_dispatch build/test_advanced build/test_scheduler_final: build/test_%: tests/test_%.c build/libscheduler.a $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libscheduler.a $(LDFLAGS) -o $@

.PHONY: test-scheduler
test-scheduler: build/test_scheduler build/test_service build/test_loop build/test_dispatch build/test_policy build/test_advanced build/test_scheduler_final build/scheduler
	./build/test_scheduler
	./build/test_service
	./build/test_loop
	./build/test_dispatch
	./build/test_policy
	./build/test_advanced
	./build/test_scheduler_final
	python3 tests/test_scheduler_network.py ./build/scheduler

build/worker: src/worker/main.c build/worker_config.o build/worker_loop.o build/worker_discovery.o build/worker_network.o build/worker_cache.o build/librunner.a $(COMMON) $(RUN_HEADERS) $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_config.o build/worker_loop.o build/worker_discovery.o build/worker_network.o build/worker_cache.o build/librunner.a $(COMMON) $(LDFLAGS) -o $@

.PHONY: test-worker-discovery
test-worker-discovery: build/test_worker_discovery build/worker
	python3 tests/test_worker_discovery.py ./build/test_worker_discovery ./build/worker

build/test_worker_cache: tests/test_worker_cache.c build/worker_cache.o $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_cache.o $(LDFLAGS) -o $@

build/job_fixture: tests/programs/job.c | build
	$(CC) $(CPPFLAGS) $(WORKLOAD_CFLAGS) $< -o $@

build/counter: tests/programs/counter.c | build
	$(CC) $(CPPFLAGS) $(WORKLOAD_CFLAGS) $< -o $@

build/test_worker_config: tests/test_worker_config.c build/worker_config.o $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_config.o $(LDFLAGS) -o $@

build/test_worker_loop: tests/test_worker_loop.c build/worker_loop.o build/librunner.a $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_loop.o build/librunner.a $(LDFLAGS) -o $@

build/test_runner: tests/test_runner.c build/librunner.a $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/librunner.a $(LDFLAGS) -o $@

build/test_start_ack: tests/test_start_ack.c build/worker_loop.o build/librunner.a build/libscheduler.a $(RUN_HEADERS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_loop.o build/librunner.a build/libscheduler.a $(LDFLAGS) -o $@

build/worker_runner: tools/worker_runner.c build/librunner.a $(RUN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/librunner.a $(LDFLAGS) -o $@

.PHONY: test-worker
test-worker: build/worker build/scheduler build/counter build/job_fixture build/test_runner build/test_start_ack build/test_worker_cache build/worker_runner build/test_worker_config build/test_worker_loop
	./build/test_worker_cache
	./build/test_worker_config
	./build/test_worker_loop ./build/job_fixture
	./build/test_runner ./build/job_fixture
	./build/test_start_ack ./build/job_fixture
	python3 tests/test_runner.py ./build/worker_runner ./build/job_fixture
	python3 tests/test_worker_network.py ./build/worker ./build/scheduler ./build/counter

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
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/worker_config.o build/worker_loop.o build/libscheduler.a build/librunner.a $(COMMON) $(LDFLAGS) -o $@

test: test-scheduler build/test_protocol build/test_discovery build/test_network build/test_policy build/test_integration
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
