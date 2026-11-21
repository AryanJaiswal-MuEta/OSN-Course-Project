#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "worker_cache.h"

static int valid(const char *key)
{
    size_t i;
    if(!key||!key[0]||key[0]=='.') return 0;
    for(i=0;i<CONTENT_LEN;i++){
        unsigned char ch=(unsigned char)key[i];
        if(!ch) return 1;
        if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_'||ch=='.')) return 0;
    }
    return 0;
}
static struct worker_cache_entry *find(struct worker_cache *c,const char *key)
{
    struct worker_cache_entry *p;
    for(p=c->head;p;p=p->next) if(!strcmp(p->key,key)) return p;
    return NULL;
}
static void forget(struct worker_cache *c,struct worker_cache_entry *p)
{
    struct worker_cache_entry **link;
    for(link=&c->head;*link&&*link!=p;link=&(*link)->next){}
    if(!*link) return;
    *link=p->next; c->size-=p->size;
    if(c->changed) c->changed(p->key,0,c->arg);
    free(p);
}

int worker_cache_evict(struct worker_cache *c,const char *protect)
{
    struct worker_cache_entry *p,*old;
    while(c->size>c->limit){
        old=NULL;
        for(p=c->head;p;p=p->next)
            if(!p->pins&&(!protect||strcmp(p->key,protect))&&(!old||p->used<old->used)) old=p;
        if(!old) return 1; // Active/new file bachaana hai; temporary over-budget allowed.
        if(unlinkat(c->fd,old->key,0)<0&&errno!=ENOENT) return -1;
        forget(c,old);
    }
    return 0;
}

int worker_cache_init(struct worker_cache *c,const char *dir,uint64_t limit)
{
    struct stat st;
    struct flock lock={0};
    struct dirent *e;
    struct worker_cache_entry *p;
    DIR *scan=NULL;
    char *resolved;
    int copy;
    memset(c,0,sizeof(*c)); c->fd=c->lock_fd=-1; c->limit=limit;
    if(!dir||!limit){ errno=EINVAL; goto fail; }
    if(mkdir(dir,0700)<0&&errno!=EEXIST) goto fail;
    c->fd=open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(c->fd<0||fstat(c->fd,&st)<0||st.st_uid!=geteuid()||(st.st_mode&0077)){ errno=EPERM; goto fail; }
    resolved=realpath(dir,NULL);
    if(!resolved) goto fail;
    if(strlen(resolved)+CONTENT_LEN+2>=sizeof(c->dir)){ free(resolved); errno=ENAMETOOLONG; goto fail; }
    strcpy(c->dir,resolved); free(resolved);
    c->lock_fd=openat(c->fd,".lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET;
    if(c->lock_fd<0||fcntl(c->lock_fd,F_SETLK,&lock)<0) goto fail;
    copy=dup(c->fd); if(copy<0) goto fail;
    scan=fdopendir(copy); if(!scan){ close(copy); goto fail; }
    while((e=readdir(scan))){
        if(!strncmp(e->d_name,".part-",6)){ unlinkat(c->fd,e->d_name,0); continue; }
        if(!valid(e->d_name)||fstatat(c->fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)<0||
           !S_ISREG(st.st_mode)||st.st_size<0) continue;
        if((uint64_t)st.st_size>UINT64_MAX-c->size) goto fail;
        p=calloc(1,sizeof(*p)); if(!p) goto fail;
        strcpy(p->key,e->d_name); p->size=(uint64_t)st.st_size;
        p->used=st.st_mtime>0?(uint64_t)st.st_mtime*1000000000:0;
#ifdef __APPLE__
        p->used+=(uint64_t)st.st_mtimespec.tv_nsec;
#else
        p->used+=(uint64_t)st.st_mtim.tv_nsec;
#endif
        if(p->used>c->clock) c->clock=p->used;
        p->next=c->head; c->head=p; c->size+=p->size;
    }
    closedir(scan); scan=NULL;
    if(worker_cache_evict(c,NULL)<0) goto fail;
    return 0;
fail:
    if(scan) closedir(scan);
    worker_cache_free(c);
    return -1;
}

void worker_cache_free(struct worker_cache *c)
{
    struct worker_cache_entry *p,*next;
    for(p=c->head;p;p=next){ next=p->next; free(p); }
    if(c->lock_fd>=0) close(c->lock_fd);
    if(c->fd>=0) close(c->fd);
    c->fd=c->lock_fd=-1; c->head=NULL;
}

int worker_cache_pin(struct worker_cache *c,const char *key,char *path,size_t cap)
{
    struct worker_cache_entry *p;
    struct stat st;
    if(!valid(key)) return -1;
    p=find(c,key);
    if(!p) return 1;
    if(fstatat(c->fd,key,&st,AT_SYMLINK_NOFOLLOW)<0||!S_ISREG(st.st_mode)||
       (uint64_t)st.st_size!=p->size){
        if(!p->pins) forget(c,p);
        else if(c->changed) c->changed(key,0,c->arg);
        return 1;
    }
    if(strlen(c->dir)+strlen(key)+2>cap) return -1;
    snprintf(path,cap,"%s/%s",c->dir,key); p->pins++;
    return 0;
}
void worker_cache_unpin(struct worker_cache *c,const char *key)
{
    struct worker_cache_entry *p=find(c,key);
    if(p&&p->pins) p->pins--;
    worker_cache_evict(c,NULL);
}
void worker_cache_touch(struct worker_cache *c,const char *key)
{
    struct worker_cache_entry *p=find(c,key);
    if(p){ p->used=++c->clock; utimensat(c->fd,key,NULL,AT_SYMLINK_NOFOLLOW); }
}

int worker_cache_begin(struct worker_cache *c,struct cache_transfer *t,const char *key,
                       uint64_t size,int id,uint64_t run)
{
    if(!valid(key)||id<=0||!run||size>64u*1024u*1024u) return -1;
    memset(t,0,sizeof(*t)); t->fd=-1;
    snprintf(t->temp,sizeof(t->temp),".part-%d-%llu",id,(unsigned long long)run);
    strcpy(t->key,key); t->size=size;
    t->fd=openat(c->fd,t->temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0700);
    return t->fd<0?-1:0;
}
int worker_cache_write(struct cache_transfer *t,uint64_t off,const void *buf,size_t size)
{
    size_t done=0;
    ssize_t n;
    if(t->fd<0||off!=t->received||size>t->size-t->received||(!buf&&size)) return -1;
    while(done<size){
        n=write(t->fd,(const char *)buf+done,size-done);
        if(n<0&&errno==EINTR) continue;
        if(n<=0) return -1;
        done+=(size_t)n;
    }
    t->received+=size;
    return 0;
}

static int same_bytes(int fd,int old,uint64_t size)
{
    unsigned char a[16384],b[16384];
    unsigned char *buf[2]={a,b};
    int fds[2]={fd,old},i;
    struct stat st;
    uint64_t off;
    size_t count,done;
    ssize_t n;
    for(i=0;i<2;i++)
        if(fstat(fds[i],&st)<0||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size!=size) return 0;
    for(off=0;off<size;off+=count){
        count=size-off>sizeof(a)?sizeof(a):(size_t)(size-off);
        for(i=0;i<2;i++){
            done=0;
            while(done<count){
                n=pread(fds[i],buf[i]+done,count-done,(off_t)(off+done));
                if(n<0&&errno==EINTR) continue;
                if(n<=0) return 0;
                done+=(size_t)n;
            }
        }
        // Short read / signal ko content mismatch samajh ke healthy session
        // disconnect nahi karna. Dono complete chunks ke bytes compare karo.
        if(memcmp(a,b,count)) return 0;
    }
    return 1;
}

int worker_cache_end(struct worker_cache *c,struct cache_transfer *t,char *path,size_t cap)
{
    struct worker_cache_entry *p;
    int fd,old,err;
    if(t->fd<0||t->received!=t->size) return -1;
    if(close(t->fd)<0){ t->fd=-1; return -1; } t->fd=-1;
    p=find(c,t->key);
    if(p){
        // Same identity pe different bytes accept nahi karenge. Hash function
        // Member 3 ke identity service ka kaam hai; yahan exact byte compare hai.
        if(p->size!=t->size) return -1;
        fd=openat(c->fd,t->temp,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        old=openat(c->fd,t->key,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd>=0&&old<0&&errno==ENOENT){
            close(fd);
            // Active job ki local copy safe hai; deleted cache entry ko full
            // retransmission se repair kar sakte hain, existing pins preserve karke.
            if(renameat(c->fd,t->temp,c->fd,t->key)<0) return -1;
            if(c->changed) c->changed(t->key,1,c->arg);
            goto pinned;
        }
        if(fd<0||old<0){ if(fd>=0) close(fd); if(old>=0) close(old); return -1; }
        err=same_bytes(fd,old,t->size);
        close(fd); close(old); if(!err) return -1;
        unlinkat(c->fd,t->temp,0);
    }else{
        if(t->size>UINT64_MAX-c->size) return -1;
        p=calloc(1,sizeof(*p)); if(!p) return -1;
        if(renameat(c->fd,t->temp,c->fd,t->key)<0){ free(p); return -1; }
        strcpy(p->key,t->key); p->size=t->size; p->used=++c->clock;
        p->next=c->head; c->head=p; c->size+=p->size;
        if(c->changed) c->changed(p->key,1,c->arg);
    }
pinned:
    t->temp[0]=0;
    err=worker_cache_pin(c,t->key,path,cap);
    if(!err) worker_cache_evict(c,t->key);
    return err;
}
void worker_cache_abort(struct worker_cache *c,struct cache_transfer *t)
{
    if(t->fd>=0) close(t->fd);
    if(t->temp[0]) unlinkat(c->fd,t->temp,0);
    t->fd=-1; t->temp[0]=0;
}
