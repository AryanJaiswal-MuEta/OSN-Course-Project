#ifndef OSN_RUNNER_H
#define OSN_RUNNER_H

#include <sys/types.h>
#include "model.h"

enum run_err { RUN_OK=0,RUN_BAD_INPUT,RUN_NO_MEMORY,RUN_NOT_FOUND,RUN_IO,RUN_BUSY };
enum stop_reason { STOP_NONE,STOP_CANCEL,STOP_TIMEOUT,STOP_MEMORY };

struct run_job {
    int id;
    uint64_t run_id;
    pid_t pid;
    char dir[128];
    int dir_fd;
    int out_fd;
    int err_fd;
    off_t offset;
    uint64_t start_ms;
    struct job_req req;
    struct telemetry usage;
    enum job_state state;
    enum stop_reason stop;
    int reaped;
    int finished;
    int status;
    int setup_stage;
    int setup_errno;
    unsigned char err_buf[2*sizeof(int)];
    size_t err_count;
    char reason[160];
    struct run_job *next;
};

struct runner {
    struct run_job *head;
    uid_t uid;
    gid_t gid;
    int drop_user;
};

// Callback ne 0 return kiya toh bytes accept ho gaye. Backpressure/error pe
// nonzero return karo; offset wahi rahega aur next poll pe same bytes milenge.
typedef int (*output_fn)(int id,uint64_t run_id,const void *buf,size_t len,void *arg);

int runner_init(struct runner *r);
int runner_start(struct runner *r,int id,uint64_t run_id,
                 const struct job_req *req,char *const args[]);
struct run_job *runner_find(struct runner *r,int id,uint64_t run_id);
int runner_poll(struct runner *r,output_fn output,void *arg);
int runner_cancel(struct runner *r,int id,uint64_t run_id);
int runner_remove(struct runner *r,int id,uint64_t run_id);
void runner_free(struct runner *r);
uint64_t runner_now_ms(void);
const char *runner_error(int err);

#endif
