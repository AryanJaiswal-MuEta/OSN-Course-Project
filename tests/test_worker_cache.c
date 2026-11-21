#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "worker_cache.h"

#define CHECK(x) do{ checks++; if(!(x)){ fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } }while(0)
static int checks;
static void put(struct worker_cache *c,const char *key,const char *data,int id)
{
    struct cache_transfer t={.fd=-1};
    char path[PATH_LEN];
    CHECK(worker_cache_begin(c,&t,key,strlen(data),id,1)==0);
    CHECK(worker_cache_write(&t,1,data,1)<0); // Out-of-order chunk rejected.
    CHECK(worker_cache_write(&t,0,data,strlen(data))==0);
    CHECK(worker_cache_end(c,&t,path,sizeof(path))==0);
}

static void large_duplicate(void)
{
    char dir[]="/tmp/osn_cache_bytes_XXXXXX",path[PATH_LEN],lock[PATH_LEN];
    unsigned char data[16421];
    struct worker_cache c;
    struct cache_transfer t={.fd=-1};
    memset(data,0x5a,sizeof(data));
    CHECK(mkdtemp(dir)!=NULL&&worker_cache_init(&c,dir,65536)==0);
    CHECK(worker_cache_begin(&c,&t,"large",sizeof(data),10,1)==0);
    CHECK(worker_cache_write(&t,0,data,100)==0);
    CHECK(worker_cache_write(&t,100,data+100,sizeof(data)-100)==0);
    CHECK(worker_cache_end(&c,&t,path,sizeof(path))==0);
    CHECK(worker_cache_begin(&c,&t,"large",sizeof(data),11,1)==0);
    CHECK(worker_cache_write(&t,0,data,sizeof(data))==0);
    CHECK(worker_cache_end(&c,&t,path,sizeof(path))==0);
    CHECK(c.size==sizeof(data));
    data[16384]^=1;
    CHECK(worker_cache_begin(&c,&t,"large",sizeof(data),12,1)==0);
    CHECK(worker_cache_write(&t,0,data,sizeof(data))==0);
    CHECK(worker_cache_end(&c,&t,path,sizeof(path))<0);
    worker_cache_abort(&c,&t);
    worker_cache_unpin(&c,"large"); worker_cache_unpin(&c,"large");
    CHECK(unlink(path)==0);
    worker_cache_free(&c);
    snprintf(lock,sizeof(lock),"%s/.lock",dir); CHECK(unlink(lock)==0&&rmdir(dir)==0);
}
int main(void)
{
    char dir[]="/tmp/osn_cache_test_XXXXXX",path[PATH_LEN],lock[PATH_LEN];
    struct worker_cache c;
    struct cache_transfer t={.fd=-1};
    CHECK(mkdtemp(dir)!=NULL);
    CHECK(worker_cache_init(&c,dir,6)==0);
    put(&c,"A","aaa",1); worker_cache_unpin(&c,"A");
    put(&c,"B","bbb",2); worker_cache_unpin(&c,"B");
    CHECK(worker_cache_pin(&c,"A",path,sizeof(path))==0);
    worker_cache_touch(&c,"A"); worker_cache_unpin(&c,"A");
    put(&c,"C","ccc",3);
    CHECK(worker_cache_pin(&c,"B",path,sizeof(path))==1);
    CHECK(worker_cache_pin(&c,"A",path,sizeof(path))==0);
    put(&c,"D","123456789",4);
    CHECK(c.size==15); // Active + newly stored files protected even over budget.
    worker_cache_unpin(&c,"D"); CHECK(c.size==6);
    CHECK(worker_cache_begin(&c,&t,"A",3,5,1)==0);
    CHECK(worker_cache_write(&t,0,"BAD",3)==0);
    CHECK(worker_cache_end(&c,&t,path,sizeof(path))<0);
    worker_cache_abort(&c,&t);
    CHECK(worker_cache_begin(&c,&t,"../escape",1,6,1)<0);
    worker_cache_unpin(&c,"A"); worker_cache_unpin(&c,"C");
    worker_cache_free(&c);
    CHECK(worker_cache_init(&c,dir,6)==0);
    CHECK(worker_cache_pin(&c,"A",path,sizeof(path))==0);
    CHECK(unlink(path)==0); worker_cache_unpin(&c,"A");
    CHECK(worker_cache_pin(&c,"A",path,sizeof(path))==1);
    CHECK(worker_cache_pin(&c,"C",path,sizeof(path))==0);
    CHECK(unlink(path)==0); worker_cache_unpin(&c,"C");
    worker_cache_free(&c);
    snprintf(lock,sizeof(lock),"%s/.lock",dir); CHECK(unlink(lock)==0); CHECK(rmdir(dir)==0);
    large_duplicate();
    printf("PASS cache LRU, pinning, collision, traversal, restart, missing file: %d checks\n",checks);
    return 0;
}
