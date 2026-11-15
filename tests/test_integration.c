/**
 * @file test_integration.c
 * @brief Integration tests for complete pipeline: submission, dispatch, execution, and output collection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "model.h"
#include "protocol.h"
#include "scheduler.h"
#include "scheduler_service.h"
#include "transport.h"

static int checks = 0;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static int test_pipeline_submission_and_dispatch(void)
{
    struct scheduler s;
    sched_init(&s);

    struct sched_service svc;
    CHECK(sched_service_init(&svc, &s, NULL, NULL, NULL) == S_OK);

    // 1. Register worker node
    struct resource cap = {4, 2048, 0};
    int wid = 0;
    CHECK(sched_register_worker(&svc, cap, 1000, &wid) == S_OK);
    CHECK(wid > 0);

    // 2. Query nodes
    struct sched_node nodes[4];
    size_t count = 0;
    CHECK(sched_nodes_get(&svc, nodes, 4, &count) == S_OK);
    CHECK(count == 1);
    CHECK(nodes[0].id == wid);
    CHECK(nodes[0].free.cores == 4);
    CHECK(nodes[0].free.ram == 2048);

    // 3. Submit a job
    struct job_req req;
    memset(&req, 0, sizeof(req));
    strcpy(req.path, "/bin/echo");
    req.need = (struct resource){2, 512, 0};
    req.time_limit = 30;
    req.prio = 2;

    int jid = 0;
    CHECK(sched_submit(&svc, "alice", &req, 1050, &jid) == S_OK);
    CHECK(jid > 0);

    // 4. Query status (should be QUEUED)
    struct sched_status st;
    CHECK(sched_status_get(&svc, jid, &st) == S_OK);
    CHECK(st.state == QUEUED);

    // 5. Dispatch job to worker
    uint64_t run_id = 0;
    CHECK(job_start(&s, jid, wid, 1100, &run_id) == S_OK);

    // Verify worker resources deducted
    struct worker *w = worker_find(&s, wid);
    CHECK(w != NULL);
    CHECK(w->used.cores == 2);
    CHECK(w->used.ram == 512);

    // Status is now RUNNING
    CHECK(sched_status_get(&svc, jid, &st) == S_OK);
    CHECK(st.state == RUNNING);
    CHECK(st.worker_id == wid);

    // 6. Complete job
    CHECK(job_finish(&s, jid, run_id, COMPLETED, "Job finished successfully", 1200) == S_OK);

    // Resources released
    CHECK(w->used.cores == 0);
    CHECK(w->used.ram == 0);

    // Final state is COMPLETED
    CHECK(sched_status_get(&svc, jid, &st) == S_OK);
    CHECK(st.state == COMPLETED);

    sched_free(&s);
    return 1;
}

static int test_pipeline_worker_failure_handling(void)
{
    struct scheduler s;
    sched_init(&s);

    struct sched_service svc;
    CHECK(sched_service_init(&svc, &s, NULL, NULL, NULL) == S_OK);

    // Register worker
    struct resource cap = {2, 1024, 0};
    int wid = 0;
    CHECK(sched_register_worker(&svc, cap, 1000, &wid) == S_OK);

    // Submit and start job
    struct job_req req;
    memset(&req, 0, sizeof(req));
    strcpy(req.path, "/bin/sleep");
    req.need = (struct resource){1, 256, 0};
    req.time_limit = 10;
    req.prio = 1;

    int jid = 0;
    CHECK(sched_submit(&svc, "bob", &req, 1050, &jid) == S_OK);

    uint64_t run_id = 0;
    CHECK(job_start(&s, jid, wid, 1100, &run_id) == S_OK);

    // Transition worker to SUSPECTED when heartbeat delayed
    CHECK(worker_set_state(&s, wid, SUSPECTED) == S_OK);
    struct worker *w = worker_find(&s, wid);
    CHECK(w->state == SUSPECTED);

    // Complete job
    CHECK(job_finish(&s, jid, run_id, COMPLETED, "Finished", 1300) == S_OK);

    // Now transition to DEAD
    CHECK(worker_set_state(&s, wid, DEAD) == S_OK);
    CHECK(w->state == DEAD);

    sched_free(&s);
    return 1;
}

int main(void)
{
    int passed = 0;
    if (test_pipeline_submission_and_dispatch()) {
        puts("PASS integration submit -> dispatch -> finish lifecycle");
        passed++;
    }
    if (test_pipeline_worker_failure_handling()) {
        puts("PASS integration worker failure state transitions");
        passed++;
    }

    printf("Integration tests: %d groups passed, %d checks\n", passed, checks);
    return passed == 2 ? 0 : 1;
}

