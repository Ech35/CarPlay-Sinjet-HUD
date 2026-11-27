#ifndef U5_NAV_IPC_H
#define U5_NAV_IPC_H
#include <stdint.h>
#define NAV_MAGIC 0x55354e39u
#define NAV_PATH "/tmp/u5hud/nav-r9"
#define NAV_BYTES 532480u
#define NAV_SLOTS 64u
#define NAV_MAX 8192u
struct NavSlot {
    uint32_t stamp,epoch,size,seconds,milliseconds;
    unsigned char data[NAV_MAX];
};
struct NavRing {
    uint32_t magic,owner,state_seq,epoch,pid,active,write_seq,dropped,oversize;
    uint32_t reserved[23];
    struct NavSlot slots[NAV_SLOTS];
};
struct NavState {uint32_t epoch,pid,active,write_seq,dropped,oversize;};
_Static_assert(sizeof(struct NavRing)<=NAV_BYTES,"navigation mailbox exceeds fixed RAM budget");
static inline int nav_state(const struct NavRing *s,struct NavState *out){
    for(unsigned i=0;i<8;i++){
        unsigned seq=__atomic_load_n(&s->state_seq,__ATOMIC_ACQUIRE);if(seq&1)continue;
        out->epoch=s->epoch;out->pid=s->pid;out->active=s->active;
        out->write_seq=__atomic_load_n(&s->write_seq,__ATOMIC_ACQUIRE);
        out->dropped=__atomic_load_n(&s->dropped,__ATOMIC_RELAXED);
        out->oversize=__atomic_load_n(&s->oversize,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(seq==__atomic_load_n(&s->state_seq,__ATOMIC_RELAXED))return 1;
    }return 0;
}
#endif
