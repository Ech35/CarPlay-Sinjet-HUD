#ifndef U6_DIAG_IO_H
#define U6_DIAG_IO_H
#include "u6diag.h"

static inline unsigned read_word(const uint32_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline void write_word(uint32_t *p,unsigned v) { __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static inline unsigned add_count(uint32_t *p) { return __atomic_add_fetch(p,1,__ATOMIC_RELAXED); }
static inline unsigned be16(const unsigned char *p) { return ((unsigned)p[0]<<8)|p[1]; }

static long syscall6(unsigned number,long a,long b,long c,long d,long e,long f) {
    register long r0 __asm__("r0")=a;
    register long r1 __asm__("r1")=b;
    register long r2 __asm__("r2")=c;
    register long r3 __asm__("r3")=d;
    register long r4 __asm__("r4")=e;
    register long r5 __asm__("r5")=f;
    register long r7 __asm__("r7")=number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r5),"r"(r7) : "memory","cc");
    return r0;
}

static struct Diag *map_diag(struct Diag **cache) {
    struct Diag *d=__atomic_load_n(cache,__ATOMIC_ACQUIRE);
    if (d) return d;
    int fd=syscall6(5,(long)DIAG_PATH,2,0,0,0,0);
    if (fd<0) return 0;
    unsigned magic=0;
    long bytes=syscall6(3,fd,(long)&magic,sizeof(magic),0,0,0);
    long addr=bytes==sizeof(magic) && magic==DIAG_MAGIC ? syscall6(192,0,sizeof(*d),3,1,fd,0) : -1;
    syscall6(6,fd,0,0,0,0,0);
    if ((unsigned long)addr>=(unsigned long)-4095) return 0;
    d=(struct Diag *)(uintptr_t)addr;
    struct Diag *expected=0;
    if (!__atomic_compare_exchange_n(cache,&expected,d,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE)) {
        syscall6(91,addr,sizeof(*d),0,0,0,0);
        return expected;
    }
    return d;
}

static unsigned monotonic_ms(void) {
    long ts[2]={0,0};
    syscall6(263,1,(long)ts,0,0,0,0);
    return 1000u*(unsigned)ts[0]+(unsigned)ts[1]/1000000u;
}

static void write_event_for(struct Diag *d,unsigned session,unsigned event,unsigned message,
                            unsigned detail,unsigned extra,const unsigned char *raw,unsigned size) {
    if (!d || !read_word(&d->enabled)) return;
    unsigned seq=add_count(&d->head);
    struct EventRecord *r=&d->records[(seq-1)&(DIAG_CAPACITY-1)];
    __atomic_store_n(&r->stamp,0,__ATOMIC_SEQ_CST);
    r->session=session;
    r->time_ms=monotonic_ms();
    r->type=event; r->message=message; r->detail=detail; r->extra=extra; r->frame_bytes=size;
    r->raw_bytes=raw ? (size<DIAG_RAW_BYTES?size:DIAG_RAW_BYTES) : 0;
    for (unsigned i=0;i<r->raw_bytes;++i) r->raw[i]=raw[i];
    write_word(&r->stamp,seq);
}

static inline void write_event(struct Diag *d,unsigned event,unsigned message,unsigned detail,
                               unsigned extra,const unsigned char *raw,unsigned size) {
    if (d) write_event_for(d,read_word(&d->session),event,message,detail,extra,raw,size);
}
#endif
