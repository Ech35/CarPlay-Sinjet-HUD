#ifndef U6_CARPLAY_GATE_H
#define U6_CARPLAY_GATE_H
#include "u6diag.h"

struct CarplayGate {
    const char *reason;
    unsigned session, phase, pid, auth_complete, callback_ready, marker;
    uint64_t process_start;
    int error;
    char comm[64], executable[256];
};

/* comm is a mutable task label. Validate the executable of the PID supplied
 * by the authenticated native iAP2 session instead of requiring that label. */
static inline int u6_carplay_gate(const struct Diag *d, struct CarplayGate *g) {
    memset(g,0,sizeof(*g));g->reason="state_unavailable";
    g->marker=access(CARPLAY_MARKER,F_OK)==0;
    if(!d)return 0;
    g->session=__atomic_load_n(&d->session,__ATOMIC_ACQUIRE);
    g->phase=__atomic_load_n(&d->phase,__ATOMIC_ACQUIRE);
    g->pid=__atomic_load_n(&d->producer_pid,__ATOMIC_ACQUIRE);
    g->auth_complete=g->session&&__atomic_load_n(&d->auth_complete_session,__ATOMIC_ACQUIRE)==g->session;
    g->callback_ready=__atomic_load_n(&d->callback_ready,__ATOMIC_ACQUIRE)!=0;
    if(!g->session){g->reason="waiting_session";return 0;}
    if(!g->pid){g->reason="waiting_producer";return 0;}
    if(g->phase!=SUBMITTED&&g->phase!=RECEIVING){g->reason="waiting_subscription";return 0;}
    if(!g->auth_complete){g->reason="waiting_auth";return 0;}
    if(!g->callback_ready){g->reason="waiting_callback";return 0;}
    char path[64];snprintf(path,sizeof(path),"/proc/%u/comm",g->pid);
    FILE *f=fopen(path,"r");
    if(f){if(fgets(g->comm,sizeof(g->comm),f))g->comm[strcspn(g->comm,"\r\n")]=0;fclose(f);}
    g->process_start=ble_process_start(g->pid);
    if(!g->process_start){g->reason="process_not_live";return 0;}
    snprintf(path,sizeof(path),"/proc/%u/exe",g->pid);
    ssize_t n=readlink(path,g->executable,sizeof(g->executable)-1);
    if(n<0){g->reason="process_exe_unavailable";g->error=errno;return 0;}
    g->executable[n]=0;
    if(n==(ssize_t)sizeof(g->executable)-1){g->reason="process_exe_truncated";return 0;}
    const char *name=strrchr(g->executable,'/');name=name?name+1:g->executable;
    if(strcmp(name,"lylinkapp")){g->reason="process_exe_mismatch";return 0;}
    if(!g->marker){g->reason="waiting_carplay_marker";return 0;}
    if(g->session!=__atomic_load_n(&d->session,__ATOMIC_ACQUIRE)||
       g->pid!=__atomic_load_n(&d->producer_pid,__ATOMIC_ACQUIRE)||
       g->phase!=__atomic_load_n(&d->phase,__ATOMIC_ACQUIRE)||
       __atomic_load_n(&d->auth_complete_session,__ATOMIC_ACQUIRE)!=g->session||
       !__atomic_load_n(&d->callback_ready,__ATOMIC_ACQUIRE)){
        g->reason="session_changed";return 0;
    }
    g->reason="ready";return 1;
}

static inline void u6_carplay_gate_json(const struct CarplayGate *g) {
    fputs(",\"gate_reason\":",stdout);ble_json_string(g->reason);
    printf(",\"gate_errno\":%d,\"gate_session\":%u,\"gate_iap_phase\":%u,\"gate_pid\":%u,"
           "\"gate_start_ticks\":%llu,\"gate_carplay_marker\":%s,\"gate_comm\":",
           g->error,g->session,g->phase,g->pid,(unsigned long long)g->process_start,g->marker?"true":"false");
    ble_json_string(g->comm);fputs(",\"gate_executable\":",stdout);ble_json_string(g->executable);
}

static inline void u6_carplay_gate_dump(const struct Diag *d) {
    struct CarplayGate g;int ready=u6_carplay_gate(d,&g);
    printf("\n[hud_carplay_gate]\nready=%d\nreason=%s\nerrno=%d\nsession=%u\niap_phase=%u\nproducer_pid=%u\n"
           "auth_complete=%u\ncallback_ready=%u\ncarplay_marker=%u\nprocess_start_ticks=%llu\nprocess_comm=",
           ready,g.reason,g.error,g.session,g.phase,g.pid,g.auth_complete,g.callback_ready,g.marker,(unsigned long long)g.process_start);
    ble_json_string(g.comm);fputs("\nprocess_executable=",stdout);ble_json_string(g.executable);
    puts("\n[end_hud_carplay_gate]");
}
#endif
