/* Bounded, logging-independent mailbox between iAP2 and the HUD worker. */
static struct NavRing *nav_mapping(struct Local *s) {
    if (s->nav) return s->nav;
    int fd=(int)syscall6(5,(long)NAV_PATH,2,0,0,0,0);
    if (fd<0) return 0;
    unsigned magic=0;
    long count=syscall6(3,fd,(long)&magic,4,0,0,0);
    long address=count==4 && magic==NAV_MAGIC ? syscall6(192,0,NAV_BYTES,3,1,fd,0) : -1;
    syscall6(6,fd,0,0,0,0,0);
    if ((unsigned long)address>=(unsigned long)-4095) return 0;
    if (!s->pid) s->pid=(unsigned)syscall6(20,0,0,0,0,0,0);
    s->nav=(struct NavRing *)(uintptr_t)address;
    return s->nav;
}

static int nav_lock(struct NavRing *r,unsigned pid) {
    unsigned expected=0;
    if (__atomic_compare_exchange_n(&r->owner,&expected,pid,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    if (expected && expected!=pid && syscall6(37,expected,0,0,0,0,0)==-3 &&
        __atomic_compare_exchange_n(&r->owner,&expected,pid,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    add_count(&r->dropped);
    return 0;
}

static void nav_lifecycle(struct Local *s,unsigned begin,unsigned active) {
    struct NavRing *r=nav_mapping(s);
    if (!r || !nav_lock(r,s->pid)) return;
    if (!begin && r->pid && r->pid!=s->pid) {
        write_word(&r->owner,0);
        return;
    }
    unsigned state_seq=read_word(&r->state_seq)|1u;
    __atomic_exchange_n(&r->state_seq,state_seq,__ATOMIC_ACQ_REL);
    if (begin || r->pid!=s->pid || (r->active && !active)) ++r->epoch;
    r->pid=s->pid;
    r->active=!!active;
    write_word(&r->state_seq,state_seq+1);
    write_word(&r->owner,0);
}

static void nav_publish(struct Local *s,const unsigned char *p,unsigned n) {
    if (!s->accepted || !s->subscribed || s->submit_result) return;
    struct NavRing *r=nav_mapping(s);
    if (!r) return;
    if (!r->pid) nav_lifecycle(s,1,1);
    unsigned epoch=read_word(&r->epoch);
    if (r->pid!=s->pid || !r->active) return;
    if (n>NAV_MAX) { add_count(&r->oversize); return; }
    if (!nav_lock(r,s->pid)) return;
    if (r->pid!=s->pid || !r->active || r->epoch!=epoch) {
        write_word(&r->owner,0);
        return;
    }
    unsigned seq=read_word(&r->write_seq)+1;
    struct NavSlot *slot=&r->slots[(seq-1)%NAV_SLOTS];
    __atomic_exchange_n(&slot->stamp,seq*2+1,__ATOMIC_ACQ_REL);
    slot->epoch=r->epoch;
    slot->size=n;
    long now[2]={0,0};
    syscall6(263,1,(long)now,0,0,0,0);
    slot->seconds=(unsigned)now[0];
    slot->milliseconds=(unsigned)now[1]/1000000u;
    for (unsigned i=0;i<n;++i) slot->data[i]=p[i];
    write_word(&slot->stamp,seq*2);
    write_word(&r->write_seq,seq);
    write_word(&r->owner,0);
}

static void nav_discontinuity(struct Local *s,unsigned oversize) {
    struct NavRing *r=nav_mapping(s);
    if (!r || r->pid!=s->pid || !r->active) return;
    add_count(oversize?&r->oversize:&r->dropped);
}
