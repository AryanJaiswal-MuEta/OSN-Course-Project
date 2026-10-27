#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "internal.h"

static int copy_program(int dir_fd,const char *path)
{
    struct stat st;
    char buf[16384];
    ssize_t n,written;
    size_t offset;
    int in,out,ok=0;
    in=open(path,O_RDONLY|O_CLOEXEC);
    if(in<0) return -1;
    if(fstat(in,&st)<0||!S_ISREG(st.st_mode)){
        close(in);
        errno=EINVAL;
        return -1;
    }
    out=openat(dir_fd,"program",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0700);
    if(out<0){ close(in); return -1; }
    while(1){
        n=read(in,buf,sizeof(buf));
        if(n<0&&errno==EINTR) continue;
        if(n<0) break;
        if(!n){ ok=1; break; }
        offset=0;
        while(offset<(size_t)n){
            written=write(out,buf+offset,(size_t)n-offset);
            if(written<0&&errno==EINTR) continue;
            if(written<=0) goto done;
            offset+=(size_t)written;
        }
    }
done:
    close(in);
    if(close(out)<0) ok=0;
    return ok?0:-1;
}

int run_prepare(struct run_job *j,const char *path)
{
    snprintf(j->dir,sizeof(j->dir),"/tmp/osn_job_%d",j->id);
    // Purana folder mile toh usko delete/reuse mat karo. Dusre worker ki
    // active job bhi ho sakti hai; caller ko collision error de denge.
    if(mkdir(j->dir,0700)<0) return -1;
    j->dir_fd=open(j->dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(j->dir_fd<0){ rmdir(j->dir); return -1; }
    j->out_fd=openat(j->dir_fd,"output.log",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(j->out_fd<0) return -1;
    if(strchr(path,'/')&&copy_program(j->dir_fd,path)<0) return -1;
    return 0;
}

static int clear_dir(int fd)
{
    struct dirent *entry;
    struct stat st;
    DIR *dir;
    int copy,child,ok=0;
    // dup shared offset use karta hai, toh directory scan se pehle reset karo.
    copy=dup(fd);
    if(copy<0) return -1;
    dir=fdopendir(copy);
    if(!dir){ close(copy); return -1; }
    rewinddir(dir);
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
        if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)<0){ ok=-1; continue; }
        if(S_ISDIR(st.st_mode)){
            child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(child<0){ ok=-1; continue; }
            if(clear_dir(child)<0) ok=-1;
            close(child);
            if(unlinkat(fd,entry->d_name,AT_REMOVEDIR)<0) ok=-1;
        }else if(unlinkat(fd,entry->d_name,0)<0) ok=-1;
    }
    closedir(dir);
    return ok;
}

int run_cleanup(struct run_job *j)
{
    int ok=0;
    if(j->out_fd>=0){ close(j->out_fd); j->out_fd=-1; }
    if(j->err_fd>=0){ close(j->err_fd); j->err_fd=-1; }
    if(j->dir_fd>=0){
        // Symlink ka target follow nahi karna; job ne banaya ho toh sirf link
        // unlink hoga. Cleanup apne opened directory fd ke andar hi chalega.
        if(clear_dir(j->dir_fd)<0) ok=-1;
        if(rmdir(j->dir)<0) ok=-1;
        close(j->dir_fd);
        j->dir_fd=-1;
    }
    return ok;
}
