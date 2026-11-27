#include "../relay-r9/nav_ipc.h"
static struct NavRing *nav_mapping(struct ProbeState *s){
    if(s->nav)return s->nav;
    int fd=linux_call(5,(long)NAV_PATH,2,0,0,0,0);if(fd<0)return 0;
    unsigned magic=0;int n=linux_call(3,fd,(long)&magic,4,0,0,0);
    if(n!=4||magic!=NAV_MAGIC){linux_call(6,fd,0,0,0,0,0);return 0;}
    long address=linux_call(192,0,NAV_BYTES,3,1,fd,0);
    linux_call(6,fd,0,0,0,0,0);
    if((unsigned long)address>=(unsigned long)-4095)return 0;
    if(!s->pid)s->pid=linux_call(20,0,0,0,0,0,0);
    s->nav=(struct NavRing *)address;return s->nav;
}
static int nav_lock(struct NavRing *r,unsigned pid){
    unsigned expected=0;
    if(__atomic_compare_exchange_n(&r->owner,&expected,pid,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    if(expected&&expected!=pid&&linux_call(37,expected,0,0,0,0,0)==-3&&
       __atomic_compare_exchange_n(&r->owner,&expected,pid,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    /* The producer never waits on the CarPlay receive path. */
    __atomic_add_fetch(&r->dropped,1,__ATOMIC_RELAXED);return 0;
}
static void nav_unlock(struct NavRing *r){__atomic_store_n(&r->owner,0,__ATOMIC_RELEASE);}
static void nav_lifecycle(struct ProbeState *s,unsigned begin,unsigned active){
    struct NavRing *r=nav_mapping(s);if(!r||!nav_lock(r,s->pid))return;
    /* A retired app must not stop the replacement app's current session. */
    if(!begin&&r->pid&&r->pid!=s->pid){nav_unlock(r);return;}
    __atomic_add_fetch(&r->state_seq,1,__ATOMIC_ACQ_REL);
    if(begin||r->pid!=s->pid||(r->active&&!active))++r->epoch;
    r->pid=s->pid;r->active=!!active;
    __atomic_add_fetch(&r->state_seq,1,__ATOMIC_RELEASE);nav_unlock(r);
}
static void nav_publish(struct ProbeState *s,const unsigned char *p,unsigned n){
    struct NavRing *r=nav_mapping(s);
    void *owner=s->ctx?ptr(s->ctx,0):0;
    if(!r||!s->accepted||!s->subscribe||s->subscribe_result||!owner||!*(unsigned *)((char *)owner+0x144))return;
    if(!r->pid)nav_lifecycle(s,1,1);
    if(r->pid!=s->pid||!r->active)return;
    if(n>NAV_MAX){__atomic_add_fetch(&r->oversize,1,__ATOMIC_RELAXED);return;}
    if(!nav_lock(r,s->pid))return;
    unsigned seq=__atomic_load_n(&r->write_seq,__ATOMIC_RELAXED)+1;
    struct NavSlot *slot=&r->slots[(seq-1)%NAV_SLOTS];
    __atomic_exchange_n(&slot->stamp,seq*2+1,__ATOMIC_ACQ_REL);
    slot->epoch=r->epoch;slot->size=n;
    int now[2]={0};linux_call(263,1,(long)now,0,0,0,0);
    slot->seconds=now[0];slot->milliseconds=(unsigned)now[1]/1000000;
    for(unsigned i=0;i<n;i++)slot->data[i]=p[i];
    __atomic_store_n(&slot->stamp,seq*2,__ATOMIC_RELEASE);
    __atomic_store_n(&r->write_seq,seq,__ATOMIC_RELEASE);nav_unlock(r);
}
static void nav_discontinuity(struct ProbeState *s,unsigned oversize){
    struct NavRing *r=nav_mapping(s);if(!r||r->pid!=s->pid||!r->active)return;
    if(oversize)__atomic_add_fetch(&r->oversize,1,__ATOMIC_RELAXED);
    else __atomic_add_fetch(&r->dropped,1,__ATOMIC_RELAXED);
}
