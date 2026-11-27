#ifndef U6_NAV_STORAGE_H
#define U6_NAV_STORAGE_H
#include "nav_ipc.h"

static inline struct NavRing *nav_open(void) {
    struct statfs fs;
    if(statfs("/tmp/u6diag",&fs) ||
       ((unsigned)fs.f_type!=0x01021994u && (unsigned)fs.f_type!=0x858458f6u)){
        errno=EROFS;return NULL;
    }
    int fd=open(NAV_PATH,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return NULL;
    if(flock(fd,LOCK_EX)){close(fd);return NULL;}
    off_t size=lseek(fd,0,SEEK_END);
    unsigned magic=0;
    if(size==NAV_BYTES && lseek(fd,0,SEEK_SET)>=0)read(fd,&magic,4);
    if(size!=NAV_BYTES || magic!=NAV_MAGIC){
        static const unsigned char zeros[4096];
        if(ftruncate(fd,NAV_BYTES) || lseek(fd,0,SEEK_SET)<0){close(fd);return NULL;}
        for(unsigned used=0;used<NAV_BYTES;){
            ssize_t count=write(fd,zeros,sizeof(zeros));
            if(count<0 && errno==EINTR)continue;
            if(count!=(ssize_t)sizeof(zeros)){int failure=count<0?errno:ENOSPC;close(fd);errno=failure;return NULL;}
            used+=(unsigned)count;
        }
    }
    struct NavRing *r=mmap(NULL,NAV_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(r==MAP_FAILED){int failure=errno;close(fd);errno=failure;return NULL;}
    __atomic_store_n(&r->magic,NAV_MAGIC,__ATOMIC_RELEASE);
    close(fd);
    return r;
}
#endif
