#include "diag_io.h"

#define CP_TRACE_STATE 0x1f100u
#define SEND_IAP_PLT 0x1f5cu

struct SendTrace {
    struct Diag *diag;
    unsigned session, prefix, sequence;
};
_Static_assert(sizeof(struct SendTrace)<=0x100u,"reserved CarPlay state area too small");

/* The native iAP2 writer adds a nine-byte link header and one-byte payload
 * checksum.  Match the exact 24-byte request handed to the native queue. */
static const unsigned char request_frame[]={
    0x40,0x40,0,24,0x52,0,0,6,0,0,COMPONENT_ID>>8,COMPONENT_ID&255,
    0,4,0,1,0,4,0,2,0,4,0,3
};

static struct Diag *find_request(const unsigned char *p,unsigned n,uintptr_t base,
                                 unsigned *session,unsigned *link_info) {
    if (!p || n<11 || be16(p)!=0xff5a || be16(p+2)!=n) return 0;
    struct SendTrace *trace=(struct SendTrace *)(base+CP_TRACE_STATE);
    struct Diag *d=map_diag(&trace->diag);
    if (!d || !read_word(&d->control_tx) || p[7]!=read_word(&d->control_channel)) return 0;
    unsigned current=read_word(&d->session), sequence=p[5];
    unsigned matched=current==trace->session && (unsigned char)(trace->sequence+1)==sequence
        ? trace->prefix : 0;
    unsigned continued=matched!=0;
    trace->session=current;
    trace->sequence=sequence;
    for (unsigned i=9;i<n-1;++i) {
        if (p[i]==request_frame[matched]) {
            if (++matched==sizeof(request_frame)) {
                trace->prefix=0;
                *session=current;
                *link_info=sequence|((unsigned)p[7]<<8)|(continued<<16);
                return d;
            }
        } else {
            /* A run of '@' can still contain the two-byte opening marker. */
            matched=p[i]!=0x40?0:(matched==2?2:1);
            continued=0;
        }
    }
    trace->prefix=matched;
    return 0;
}

int u6_carplay_send(void *cp,const unsigned char *p,unsigned n,uintptr_t base) {
    unsigned session,link_info;
    struct Diag *d=find_request(p,n,base,&session,&link_info);
    if (d) {
        add_count(&d->cp_calls);
        write_event_for(d,session,CP_SEND_ENTER,0x5200,COMPONENT_ID,link_info,0,n);
    }
    typedef int (*native_send)(void *,const unsigned char *,unsigned);
    int result=((native_send)(base+SEND_IAP_PLT))(cp,p,n);
    if (d) {
        if (read_word(&d->session)==session) {
            write_word(&d->cp_return,(unsigned)result);
            add_count(&d->cp_returns);
        }
        write_event_for(d,session,CP_SEND_RETURN,0x5200,(unsigned)result,link_info,0,n);
    }
    return result;
}

void u6_carplay_skipped(const unsigned char *p,unsigned n,uintptr_t base) {
    unsigned session,link_info;
    struct Diag *d=find_request(p,n,base,&session,&link_info);
    if (!d) return;
    add_count(&d->cp_skipped);
    write_event_for(d,session,CP_SEND_SKIPPED,0x5200,0,link_info,0,n);
}
