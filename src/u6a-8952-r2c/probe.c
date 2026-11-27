#include <stddef.h>
#include <stdint.h>
#include "diag_io.h"
#include "nav_ipc.h"

#define STATE_ADDRESS 0x31800u
#define SUBMIT_ADDRESS 0x12314u
#define PARAM_ADDRESS 0x11eecu
#define TRANSPORT_WIFI_CARPLAY 3u
#define STORAGE_LIMIT 0x10000u
#define RX_FRAME_LIMIT 8192u
#define RX_CHANNELS 4u
#define POLL_MS 250u

struct RxStream {
    unsigned used, skip, noted;
    unsigned char bytes[RX_FRAME_LIMIT];
};

struct Local {
    void *context;
    struct Diag *diag;
    struct NavRing *nav;
    unsigned session, pid, added, fallback, accepted, subscribed, submit_result;
    unsigned marker_sampled, marker_value, marker_checked_ms;
    struct RxStream rx[RX_CHANNELS];
};
_Static_assert(sizeof(struct Local)<=0x9000u,"reserved state area too small");
struct Cache { uint32_t unused, used, capacity; unsigned char *data; };

static uint32_t word_at(const void *p, unsigned offset) {
    return *(const uint32_t *)((const unsigned char *)p + offset);
}
static void *ptr_at(const void *p, unsigned offset) {
    return (void *)(uintptr_t)word_at(p, offset);
}
static void put16(unsigned char *p, unsigned value) { p[0] = value >> 8; p[1] = value; }
static struct Local *local(uintptr_t base) { return (struct Local *)(base + STATE_ADDRESS); }
static unsigned control_channel(const void *ctx) { return ((const unsigned char *)ctx)[0x20]; }

/* lyLinkApp installs transport type 3 only at syncIAP/restartIAP. The same
 * library also handles ordinary Bluetooth pairing (type 2).
 * The native context, its client, and the callback must belong together. */
static unsigned transport_type(void *ctx) {
    if (!ctx) return 0;
    void *owner=ptr_at(ctx,0);
    if (!owner || ptr_at(owner,0x104)!=ctx) return 0;
    void *impl=ptr_at(owner,0x20);
    if (!impl || ptr_at(impl,0)!=owner) return 0;
    return word_at(impl,0xe8);
}
static int link_ready(void *ctx) {
    if (transport_type(ctx)!=TRANSPORT_WIFI_CARPLAY || !ptr_at(ctx,0x48)) return 0;
    void *impl=ptr_at(ptr_at(ctx,0),0x20);
    return ptr_at(impl,0xec)!=0;
}

#include "nav_producer.h"

static int marker_present(void) { return syscall6(33,(long)CARPLAY_MARKER,0,0,0,0,0)==0; }

static void observe_marker(struct Local *s,struct Diag *d,int immediately) {
    if (!read_word(&d->enabled)) return;
    unsigned now=monotonic_ms();
    if (!immediately && s->marker_sampled && now-s->marker_checked_ms<POLL_MS) return;
    s->marker_checked_ms=now;
    unsigned present=marker_present();
    if (!s->marker_sampled || s->marker_value!=present) {
        write_word(&d->marker_seen,present);
        write_event(d,MARKER_OBSERVED,0,present,!s->marker_sampled,0,0);
    }
    s->marker_sampled=1;
    s->marker_value=present;
}

static void start_session(struct Local *s,struct Diag *d,void *ctx) {
    write_word(&d->phase,WAITING);
    s->context=ctx;
    s->fallback=s->added=s->accepted=s->subscribed=0;
    s->submit_result=0;
    if (!s->pid) s->pid=(unsigned)syscall6(20,0,0,0,0,0,0);
    s->marker_sampled=s->marker_value=0;
    for (unsigned i=0;i<RX_CHANNELS;++i) s->rx[i].used=s->rx[i].skip=s->rx[i].noted=0;
    write_word(&d->producer_pid,s->pid);
    write_word(&d->control_channel,control_channel(ctx));
    write_word(&d->auth_success,0);
    write_word(&d->marker_seen,0);
    write_word(&d->submit_return,0);
    write_word(&d->control_tx,0);
    write_word(&d->cp_calls,0);
    write_word(&d->cp_returns,0);
    write_word(&d->cp_return,0);
    write_word(&d->cp_skipped,0);
    write_word(&d->callback_ready,link_ready(ctx));
    write_word(&d->pass_mode,word_at(ptr_at(ctx,0),0x144));
    s->session=add_count(&d->session);
    nav_lifecycle(s,1,0);
    observe_marker(s,d,1);
}

/* The U5 registration format also works with the 8952 native serializer.
 * Its 6 and 7 fields are generated before this hook and already contain
 * the stock send/receive lists. Byte 0 to 5 are filled by stock code later. */
static const uint16_t nav_ids[]={0x5200,0x5203,0x5201,0x5202,0x5204};
static const unsigned char component[]="RouteGuidanceDisplayComponent";
#define COMPONENT_BYTES (4u + 8u*6u + 4u + sizeof(component))

static int listed(const unsigned char *p,unsigned n,unsigned id) {
    for (unsigned i=0;i<n;i+=2) if (be16(p+i)==id) return 1;
    return 0;
}

static unsigned extend_identity(struct Cache *cache) {
    if (!cache || !cache->data || cache->used<6 || cache->used>cache->capacity ||
        cache->used>0xffffu || cache->capacity>STORAGE_LIMIT) return INVALID_LAYOUT;
    unsigned char *p=cache->data;
    unsigned used=cache->used, offset[2]={0,0}, grow[2]={0,0};
    for (unsigned at=6;at<used;) {
        if (used-at<4) return INVALID_LAYOUT;
        unsigned n=be16(p+at), tag=be16(p+at+2);
        if (n<4 || n>used-at) return INVALID_LAYOUT;
        if (tag==30) return ALREADY_PRESENT;
        if (tag>=10 && n>=10 && be16(p+at+4)==6 && be16(p+at+6)==0 &&
            be16(p+at+8)==COMPONENT_ID) return COMPONENT_CONFLICT;
        if (tag==6 || tag==7) {
            unsigned k=tag-6;
            if (offset[k] || (n&1)) return INVALID_LAYOUT;
            offset[k]=at;
            for (unsigned i=0;i<k+2;++i)
                if (!listed(p+at+4,n-4,nav_ids[k*2+i])) grow[k]+=2;
        }
        at+=n;
    }
    if (!offset[0] || !offset[1]) return NO_LISTS;
    unsigned growth=grow[0]+grow[1]+COMPONENT_BYTES;
    if (growth>cache->capacity-used || growth>0xffffu-used) return NO_SPACE;
    unsigned k=offset[0]>offset[1]?0:1;
    for (unsigned pass=0;pass<2;++pass,k^=1) {
        unsigned at=offset[k], old=be16(p+at), end=at+old;
        if (!grow[k]) continue;
        for (unsigned i=used;i>end;--i) p[i-1+grow[k]]=p[i-1];
        for (unsigned i=0;i<k+2;++i) if (!listed(p+at+4,old-4,nav_ids[k*2+i])) {
            put16(p+end,nav_ids[k*2+i]); end+=2;
        }
        put16(p+at,old+grow[k]);
        used+=grow[k];
    }
    put16(p+used,COMPONENT_BYTES); put16(p+used+2,30);
    unsigned at=used+4;
    const uint16_t value[]={COMPONENT_ID,0,256,256,256,256,6,256,6};
    for (unsigned tag=0;tag<9;++tag) {
        unsigned n=tag==1?4+sizeof(component):6;
        put16(p+at,n); put16(p+at+2,tag);
        if (tag==1) {
            for (unsigned i=0;i<sizeof(component);++i) p[at+4+i]=component[i];
        } else put16(p+at+4,value[tag]);
        at+=n;
    }
    cache->used=at;
    return ADDED;
}

void u6_ident(void *ctx,unsigned id,struct Cache *cache,uintptr_t base) {
    if (id!=0x1d01) return;
    struct Local *s=local(base);
    unsigned transport=transport_type(ctx);
    struct Diag *d=map_diag(&s->diag);
    if (!d || !transport || transport>31) return;
    unsigned bit=1u<<transport;
    unsigned old=__atomic_fetch_or(&d->types_seen,bit,__ATOMIC_RELAXED);
    if (transport!=TRANSPORT_WIFI_CARPLAY) {
        if (!(old&bit)) write_event(d,OBSERVED_OTHER_MODE,id,transport,0,0,0);
        return;
    }
    if (s->context!=ctx || (s->accepted && !s->fallback)) start_session(s,d,ctx);
    s->added=s->accepted=s->subscribed=0;
    unsigned outcome=s->fallback?STOCK_RETRY:extend_identity(cache);
    s->added=outcome==ADDED;
    write_word(&d->phase,s->added?IDENTIFYING:STOCK_ONLY);
    if (s->added) add_count(&d->ident_sent);
    write_word(&d->callback_ready,link_ready(ctx));
    write_word(&d->last_result,outcome);
    write_event(d,IDENT_SENT,id,outcome,cache?cache->used:0,0,0);
}

static unsigned header(const unsigned char *p,unsigned n) {
    if (!p || n<6 || be16(p)!=0x4040 || be16(p+2)!=n) return 0;
    return be16(p+4);
}

static int is_navigation(unsigned id) { return id==0x5201 || id==0x5202 || id==0x5204; }

static void capture_navigation(struct Local *s,struct Diag *d,const unsigned char *p,unsigned n,
                               unsigned channel,int fragmented,int combined) {
    unsigned id=be16(p+4);
    unsigned count=0;
    unsigned component_match=0;
    unsigned i=6;
    while (n-i>=4) {
        unsigned len=be16(p+i);
        if (len<4 || len>n-i) break;
        if (id==0x5201 && be16(p+i+2)==0 && !(len&1))
            component_match|=listed(p+i+4,len-4,COMPONENT_ID);
        i+=len; ++count;
    }
    if (i!=n || !count) {
        nav_discontinuity(s,0);
        add_count(&d->nav_invalid);
        write_event(d,NAV_INVALID,id,i,channel,p,n);
        return;
    }
    if (id==0x5201) add_count(&d->nav_route);
    if (id==0x5202) add_count(&d->nav_turn);
    if (id==0x5204) add_count(&d->nav_lane);
    if (component_match) add_count(&d->route_7f01);
    if (fragmented) add_count(&d->rx_fragmented);
    if (combined) add_count(&d->rx_combined);
    if (s->subscribed) write_word(&d->phase,RECEIVING);
    nav_publish(s,p,n);
    write_event(d,NAV_RX,id,count,channel,p,n);
}

/* Observe the stock callback's data without replacing its routing.  The
 * normal control, file-transfer and accessory channels have separate buffers. */
static void observe_stream(struct Local *s,struct Diag *d,const unsigned char *p,unsigned n,unsigned channel) {
    if (!p || !n) return;
    if (channel>=RX_CHANNELS) {
        if (is_navigation(header(p,n))) capture_navigation(s,d,p,n,channel,0,0);
        return;
    }
    struct RxStream *rx=&s->rx[channel];
    int fragmented=rx->used!=0;
    unsigned completed=0;
    while (n) {
        if (rx->skip) {
            unsigned skip=rx->skip<n?rx->skip:n;
            rx->skip-=skip; p+=skip; n-=skip;
            if (!rx->skip) ++completed;
            continue;
        }
        if (rx->used<2) {
            unsigned byte=*p++;
            --n;
            if (byte==0x40) rx->bytes[rx->used++]=(unsigned char)byte;
            else { rx->used=0; fragmented=0; }
            continue;
        }
        unsigned want=rx->used<6?6:be16(rx->bytes+2);
        unsigned take=want-rx->used;
        if (take>n) take=n;
        for (unsigned j=0;j<take;++j) rx->bytes[rx->used++]=*p++;
        n-=take;
        if (rx->used<6) break;
        unsigned total=be16(rx->bytes+2), id=be16(rx->bytes+4);
        if (total<6 || total>RX_FRAME_LIMIT || !is_navigation(id)) {
            if (is_navigation(id)) {
                nav_discontinuity(s,total>RX_FRAME_LIMIT);
                if (total>RX_FRAME_LIMIT) add_count(&d->rx_oversized);
                else add_count(&d->nav_invalid);
                write_event(d,total>RX_FRAME_LIMIT?NAV_OVERSIZED:NAV_INVALID,id,total,channel,rx->bytes,6);
            }
            rx->skip=total>rx->used?total-rx->used:0;
            rx->used=rx->noted=0;
            fragmented=0;
            if (!rx->skip) ++completed;
            continue;
        }
        if (rx->used<total) continue;
        int following=n>=2 && be16(p)==0x4040;
        capture_navigation(s,d,rx->bytes,total,channel,fragmented,completed || following);
        rx->used=rx->noted=0;
        fragmented=0;
        ++completed;
    }
    if (rx->used>=6 && !rx->noted) {
        rx->noted=1;
        write_event(d,NAV_INCOMPLETE,be16(rx->bytes+4),be16(rx->bytes+2),channel,0,rx->used);
    }
}

void u6_before(void *owner,const unsigned char *p,unsigned n,unsigned channel,uintptr_t base) {
    void *ctx=ptr_at(owner,0x104);
    struct Local *s=local(base);
    if (transport_type(ctx)!=TRANSPORT_WIFI_CARPLAY) return;
    unsigned primary=control_channel(ctx);
    unsigned id=channel==primary?header(p,n):0;
    if (s->context!=ctx && id!=0x1d00) return;
    struct Diag *d=map_diag(&s->diag);
    if (!d) return;
    write_word(&d->pass_mode,word_at(owner,0x144));
    if (id==0x1d00) {
        if (s->context!=ctx || (s->accepted && !s->fallback)) start_session(s,d,ctx);
        write_event(d,IDENT_START,id,TRANSPORT_WIFI_CARPLAY,0,0,0);
    } else if (id==0x1d03 && s->added) {
        nav_lifecycle(s,0,0);
        s->fallback=1; s->added=s->accepted=s->subscribed=0;
        write_word(&d->phase,REJECTED);
        add_count(&d->rejected);
        write_event(d,IDENT_REJECT,id,n-6,0,p,n);
    }
    if (!s->added) return;
    add_count(channel==primary?&d->rx_control_chunks:&d->rx_other_chunks);
    observe_stream(s,d,p,n,channel);
}

unsigned u6_auth(void *impl,uintptr_t base) {
    void *owner=ptr_at(impl,0);
    void *ctx=owner?ptr_at(owner,0x104):0;
    if (transport_type(ctx)!=TRANSPORT_WIFI_CARPLAY || ptr_at(owner,0x20)!=impl) return 0;
    struct Local *s=local(base);
    struct Diag *d=map_diag(&s->diag);
    if (!d) return 0;
    if (s->context!=ctx) start_session(s,d,ctx);
    unsigned session=s->session;
    write_word(&d->auth_success,1);
    write_event_for(d,session,AUTH_SUCCESS,0,3,ptr_at(impl,0x24)!=0,0,0);
    return session;
}

void u6_auth_complete(unsigned session,uintptr_t base) {
    if (!session) return;
    struct Local *s=local(base);
    if (!s->context) return;
    struct Diag *d=map_diag(&s->diag);
    if (!d || session!=read_word(&d->session)) return;
    unsigned completed=read_word(&d->auth_complete_session);
    if (completed>=session) return;
    write_event_for(d,session,AUTH_COMPLETE,0,3,0,0,0);
    /* A callback returning late cannot overwrite a newer session's completion. */
    while (completed<session &&
           !__atomic_compare_exchange_n(&d->auth_complete_session,&completed,session,0,
                                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {}
}

void u6_control_handoff(void *owner,const unsigned char *p,unsigned n,unsigned channel,uintptr_t base) {
    unsigned id=header(p,n);
    if (id!=0x1d01 && id!=0x5200) return;
    struct Local *s=local(base);
    void *ctx=ptr_at(owner,0x104);
    if (!ctx || ctx!=s->context || !s->added || channel!=control_channel(ctx)) return;
    struct Diag *d=map_diag(&s->diag);
    if (!d) return;
    if (id==0x5200) {
        add_count(&d->control_tx);
        write_event(d,SUBMIT_HANDOFF,id,COMPONENT_ID,channel,p,n);
        return;
    }
    for (unsigned off=6;n-off>=4;) {
        unsigned size=be16(p+off), tag=be16(p+off+2);
        if (size<4 || size>n-off) break;
        if (tag==30) write_event(d,IDENT_COMPONENT_WIRE,id,COMPONENT_ID,channel,p+off,size);
        if (tag==6 || tag==7) write_event(d,tag==6?IDENT_TX_WIRE:IDENT_RX_WIRE,id,tag,channel,p+off,size);
        off+=size;
    }
}

typedef void (*append_fn)(void *,unsigned,const void *,int,void *,void *);
static void subscription_args(void *ctx,void *image_base) {
    static const unsigned char component_id[]={COMPONENT_ID>>8,COMPONENT_ID&255};
    append_fn append=(append_fn)((uintptr_t)image_base+PARAM_ADDRESS);
    append(ctx,0,component_id,sizeof(component_id),0,0);
    append(ctx,1,0,0,0,0); append(ctx,2,0,0,0,0); append(ctx,3,0,0,0,0);
}

static void subscribe(struct Local *s,struct Diag *d,void *ctx,uintptr_t base) {
    if (!s->accepted || s->subscribed || !link_ready(ctx)) return;
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&s->subscribed,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return;
    typedef int (*submit_fn)(void *,unsigned,void (*)(void *,void *),void *,unsigned);
    add_count(&d->subscriptions);
    write_word(&d->callback_ready,1);
    write_word(&d->pass_mode,word_at(ptr_at(ctx,0),0x144));
    write_event(d,LINK_READY,0,TRANSPORT_WIFI_CARPLAY,read_word(&d->pass_mode),0,0);
    write_event(d,SUBMIT_TRY,0x5200,COMPONENT_ID,0,0,0);
    int sent=((submit_fn)(base+SUBMIT_ADDRESS))(ctx,0x5200,subscription_args,(void *)base,0);
    s->submit_result=(unsigned)sent;
    nav_lifecycle(s,0,sent==0);
    write_word(&d->submit_return,(unsigned)sent);
    if (read_word(&d->phase)!=RECEIVING) write_word(&d->phase,sent?SEND_FAILED:SUBMITTED);
    write_event(d,SUBMIT_RESULT,0x5200,(unsigned)sent,0,0,0);
}

void u6_after(void *owner,const unsigned char *p,unsigned n,unsigned channel,uintptr_t base) {
    void *ctx=ptr_at(owner,0x104);
    struct Local *s=local(base);
    if (!ctx || s->context!=ctx || !s->added || channel!=control_channel(ctx)) return;
    unsigned id=header(p,n);
    if (!id || id==0x1d00 || id==0x1d03) return;
    struct Diag *d=map_diag(&s->diag);
    if (!d) return;
    if (id==0x1d02 && !s->accepted) {
        s->accepted=1;
        add_count(&d->accepted);
        write_word(&d->callback_ready,link_ready(ctx));
        write_word(&d->pass_mode,word_at(owner,0x144));
        write_word(&d->phase,IDENT_ACCEPTED);
        write_event(d,IDENT_RESULT,0x1d02,read_word(&d->callback_ready),read_word(&d->pass_mode),0,0);
        observe_marker(s,d,1);
    }
}

static void *loop_context(void *loop,struct Local *s) {
    if (!s->context || !loop || ((const unsigned char *)loop)[0x18]) return 0;
    void *owner=ptr_at(loop,8);
    if (!owner || ptr_at(owner,0xd0)!=loop) return 0;
    void *ctx=ptr_at(owner,0x104);
    return ctx==s->context && transport_type(ctx)==TRANSPORT_WIFI_CARPLAY ? ctx : 0;
}

void u6_service(void *loop,uintptr_t base) {
    struct Local *s=local(base);
    void *ctx=loop_context(loop,s);
    if (!ctx) return;
    struct Diag *d=map_diag(&s->diag);
    if (!d) return;
    observe_marker(s,d,0);
    if (!s->accepted || s->subscribed) return;
    unsigned ready=link_ready(ctx), session=s->session;
    write_word(&d->callback_ready,ready);
    if (!ready || session!=read_word(&d->session)) return;
    if (read_word(&d->auth_complete_session)!=session) return;
    subscribe(s,d,ctx,base);
}

/* Poll while a subscription is pending or while observing the first connection marker.
 * libc returns with the original mutex held.  -1 tells the trampoline to
 * skip the stock wake-counter decrement on a timeout without a signal. */
int u6_wait(void *cond,void *mutex,void *loop,uintptr_t base) {
    typedef int (*wait_fn)(void *,void *);
    typedef int (*timed_fn)(void *,void *,const long *);
    wait_fn wait=(wait_fn)(base+0x15ccu);
    struct Local *s=local(base);
    if (!loop_context(loop,s) || !s->accepted || !s->diag) return wait(cond,mutex);
    if (s->subscribed &&
        !(read_word(&s->diag->enabled) && !s->marker_value)) return wait(cond,mutex);
    long when[2];
    if (syscall6(263,0,(long)when,0,0,0,0)!=0) return wait(cond,mutex);
    when[1]+=POLL_MS*1000000u;
    if (when[1]>=1000000000l) { ++when[0]; when[1]-=1000000000l; }
    int result=((timed_fn)(base+0x14d0u))(cond,mutex,when);
    if (result==110) return (int32_t)word_at(loop,0x1c)>0?0:-1;
    return result?wait(cond,mutex):0;
}

void u6_closing(void *ctx,uintptr_t base) {
    struct Local *s=local(base);
    if (s->context!=ctx || !ctx) return;
    nav_lifecycle(s,0,0);
    struct Diag *d=map_diag(&s->diag);
    if (d) {
        write_event(d,LINK_END,0,read_word(&d->phase),0,0,0);
        write_word(&d->phase,WAITING);
        write_word(&d->callback_ready,0);
        write_word(&d->pass_mode,0);
    }
    s->context=0; s->fallback=s->added=s->accepted=s->subscribed=0;
}
