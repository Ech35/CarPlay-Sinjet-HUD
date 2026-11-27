#ifndef U5_BLE_SHARED_H
#define U5_BLE_SHARED_H
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "../relay-r9/nav_ipc.h"

#define BLE_MAGIC 0x55354242u
#define BLE_DEVICE_MAGIC 0x55354238u
#define BLE_BYTES 69632u
#define BLE_PATH "/tmp/u5hud/ble-r18"
#define BLE_LOCK "/tmp/u5hud/ble-worker.lock"
#define BLE_CONFIG "/mnt/UDISK/u5hud-device.bin"
#define BLE_RELAY_CONFIG "/mnt/UDISK/u5hud-relay.enabled"
#define BLE_ICON_CONFIG "/mnt/UDISK/u5hud-icons.bin"
#define BLE_PROGRAM "/mnt/customer/app/u5hud_ble"
#define BLE_PEERS 32u
#define BLE_EVENTS 512u
#define BLE_LINK_EVENTS 128u
enum { BLE_NONE, BLE_SCAN, BLE_CONNECT, BLE_TEST, BLE_RELAY_ON, BLE_RELAY_OFF };
enum { BLE_TEST_TURN, BLE_TEST_LANES };
enum { BS_IDLE, BS_SCANNING, BS_CONNECTING, BS_DISCOVERING, BS_READY, BS_TESTING, BS_ERROR };
struct BlePeer { unsigned char address[6],type,connectable; int32_t rssi; char name[64]; };
struct BleEvent { uint32_t seconds,error; char stage[32],detail[80]; };
enum BleLinkEventKind {
    BL_ATTEMPT = 1, BL_HCI_MONITOR, BL_HCI_HANDLE, BL_READY, BL_IO_ERROR,
    BL_POLL_ERROR, BL_LOCAL_CLOSE, BL_HCI_DISCONNECT, BL_MGMT_DISCONNECT,
    BL_FIRST_WRITE, BL_RETRY, BL_HCI_PARAMS, BL_HCI_PARAMS_UPDATED, BL_HCI_PARAMS_REJECTED,
    BL_OTHER_PEER, BL_FIRST_NAVIGATION, BL_HCI_HARDWARE_ERROR
};
enum BleLinkIoOperation { BL_WAIT, BL_POLL, BL_SEND, BL_RECV, BL_CONNECT, BL_WRITE_RESPONSE };
enum BleHciMonitorOperation { BL_HCI_LISTENING, BL_HCI_SOCKET, BL_HCI_FILTER, BL_HCI_BIND, BL_HCI_READ };
struct BleLinkEvent {
    uint32_t seconds, attempt, last_write_age_ms;
    uint32_t last_navigation_write_age_ms;
    uint16_t milliseconds, kind;
    int32_t values[3];
};
struct BleRequest {
    uint32_t operation;
    union { struct BlePeer peer; uint32_t test; };
};
struct BleSnapshot {
    uint32_t phase,busy,connected,request_id;
    uint32_t controller_known,controller_index,controller_settings,classic_connections;
    uint32_t error,mgmt_status,att_error,write_uuid,write_handle,write_properties;
    uint32_t test_count,last_test_seconds,clear_sent,commands_sent;
    uint32_t peer_count,event_count,event_next,clear_seen;
    char stage[32];
    struct BlePeer selected,peers[BLE_PEERS];
    struct BleEvent events[BLE_EVENTS];
    uint32_t relay_enabled,relay_input_count,relay_bad,relay_gaps,relay_stale;
    uint32_t relay_epoch,relay_sequence,relay_route_state,relay_distance;
    uint32_t relay_frames,relay_chunks,relay_last_tx,relay_last_command,relay_icon;
    uint32_t relay_core_phase,relay_retries,relay_retry_seconds,relay_clear_sent;
    char relay_source[64];
    uint32_t relay_keepalives,relay_keepalive_last_seconds,relay_input_last_seconds;
    uint32_t relay_raw_index,relay_selected_index,relay_freshness_expired;
    uint32_t relay_raw_type,relay_selected_type;
    uint32_t link_event_count,link_event_next,link_last_attempt;
    struct BleLinkEvent link_events[BLE_LINK_EVENTS];
    uint32_t last_test;
};
struct BleShared {
    uint32_t magic,request_seq,handled_seq,cancel;
    uint32_t worker_pid,worker_heartbeat,start_low,start_high;
    uint32_t clear_request,snapshot_seq;
    struct BleRequest request;
    struct BleSnapshot snapshot;
};
_Static_assert(sizeof(struct BleShared)<=BLE_BYTES,"BLE RAM page overflow");
static inline int64_t ble_now(void) {
    struct timespec t={0};return clock_gettime(CLOCK_MONOTONIC,&t)==0?(int64_t)t.tv_sec*1000+t.tv_nsec/1000000:0;
}
static inline struct BleShared *ble_map(int create,int *descriptor) {
    if(create){struct statfs s;if(statfs("/tmp/u5hud",&s)||
        ((unsigned)s.f_type!=0x01021994u&&(unsigned)s.f_type!=0x858458f6u)){errno=EROFS;return NULL;}}
    int fd=open(BLE_PATH,(create?O_RDWR|O_CREAT:O_RDONLY)|O_CLOEXEC,0600);
    if(fd<0)return NULL;
    if(create&&flock(fd,LOCK_EX)){close(fd);return NULL;}
    off_t size=lseek(fd,0,SEEK_END);
    if(size<0||(!create&&size!=BLE_BYTES)){close(fd);errno=EINVAL;return NULL;}
    uint32_t magic=0;
    if(size==BLE_BYTES){if(lseek(fd,0,SEEK_SET)<0||read(fd,&magic,4)!=4){close(fd);return NULL;}}
    if((size!=BLE_BYTES||magic!=BLE_MAGIC)&&!create){close(fd);errno=EINVAL;return NULL;}
    if(size!=BLE_BYTES||magic!=BLE_MAGIC){
        static const unsigned char zero[4096];
        if(ftruncate(fd,BLE_BYTES)||lseek(fd,0,SEEK_SET)<0){close(fd);return NULL;}
        for(unsigned i=0;i<BLE_BYTES;i+=sizeof(zero))if(write(fd,zero,sizeof(zero))!=sizeof(zero)){close(fd);errno=ENOSPC;return NULL;}
    }
    struct BleShared *s=mmap(NULL,BLE_BYTES,PROT_READ|(create?PROT_WRITE:0),MAP_SHARED,fd,0);
    if(s==MAP_FAILED){close(fd);return NULL;}
    if(create&&s->magic!=BLE_MAGIC)__atomic_store_n(&s->magic,BLE_MAGIC,__ATOMIC_RELEASE);
    if(create)flock(fd,LOCK_UN);
    if(descriptor)*descriptor=fd;else close(fd);
    return s;
}
static inline int ble_snapshot(const struct BleShared *s,struct BleSnapshot *out) {
    for(unsigned i=0;i<8;i++){
        unsigned before=__atomic_load_n(&s->snapshot_seq,__ATOMIC_ACQUIRE);
        if(before&1)continue;
        memcpy(out,&s->snapshot,sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(before==__atomic_load_n(&s->snapshot_seq,__ATOMIC_RELAXED)){
            out->stage[31]=0;out->selected.name[63]=0;
            out->relay_source[63]=0;
            if(out->peer_count>BLE_PEERS)out->peer_count=BLE_PEERS;
            if(out->event_count>BLE_EVENTS)out->event_count=BLE_EVENTS;
            for(unsigned j=0;j<BLE_PEERS;j++)out->peers[j].name[63]=0;
            for(unsigned j=0;j<BLE_EVENTS;j++){out->events[j].stage[31]=0;out->events[j].detail[79]=0;}
            return 1;
        }
    }return 0;
}
static inline uint64_t ble_process_start(unsigned pid) {
    char path[64],line[1024];snprintf(path,sizeof(path),"/proc/%u/stat",pid);
    FILE *f=fopen(path,"r");if(!f)return 0;
    int ok=fgets(line,sizeof(line),f)!=NULL;fclose(f);if(!ok)return 0;
    char *p=strrchr(line,')');if(!p||p[1]!=' ')return 0;
    p+=2;if(*p=='Z'||*p=='X')return 0;
    for(unsigned field=3;field<22;field++){p=strchr(p,' ');if(!p)return 0;while(*p==' ')p++;}
    return strtoull(p,NULL,10);
}
static inline int ble_alive(const struct BleShared *s) {
    unsigned pid=__atomic_load_n(&s->worker_pid,__ATOMIC_ACQUIRE);if(!pid)return 0;
    uint64_t expected=(uint64_t)s->start_high<<32|s->start_low;
    return expected&&ble_process_start(pid)==expected&&kill((pid_t)pid,0)==0;
}
static inline void ble_json_string(const char *p) {
    putchar('"');
    for(unsigned i=0;p[i];){
        unsigned char c=(unsigned char)p[i];
        if(c=='"'||c=='\\'){putchar('\\');putchar(c);i++;}
        else if(c<32||c==127){printf("\\u%04x",c);i++;}
        else if(c<128){putchar(c);i++;}
        else {
            unsigned n=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
            unsigned good=n;
            for(unsigned j=1;j<n;j++)if(!p[i+j]||((unsigned char)p[i+j]&0xc0)!=0x80){good=0;break;}
            if(good&&n==3&&((c==0xe0&&(unsigned char)p[i+1]<0xa0)||(c==0xed&&(unsigned char)p[i+1]>=0xa0)))good=0;
            if(good&&n==4&&((c==0xf0&&(unsigned char)p[i+1]<0x90)||(c==0xf4&&(unsigned char)p[i+1]>=0x90)))good=0;
            if(good){fwrite(p+i,1,n,stdout);i+=n;}else {fputs("\\ufffd",stdout);i++;}
        }
    }putchar('"');
}
static inline void ble_address(const unsigned char *a,char text[18]) {
    snprintf(text,18,"%02X:%02X:%02X:%02X:%02X:%02X",a[5],a[4],a[3],a[2],a[1],a[0]);
}
static inline void ble_mark_clear(void) {
    struct BleShared *s=ble_map(0,NULL);if(!s)return;
    munmap(s,BLE_BYTES);
    int fd=open(BLE_PATH,O_RDWR|O_CLOEXEC);if(fd<0)return;
    s=mmap(NULL,BLE_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(s!=MAP_FAILED){__atomic_add_fetch(&s->clear_request,1,__ATOMIC_RELEASE);munmap(s,BLE_BYTES);}
}
static inline int ble_relay_enabled(void){
    int fd=open(BLE_RELAY_CONFIG,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    char value=0;int ok=read(fd,&value,1)==1;close(fd);return ok&&value=='1';
}
static inline int ble_save_config(const char *path,const char *temporary,const void *data,size_t size){
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    int ok=write(fd,data,size)==(ssize_t)size&&fsync(fd)==0;
    if(close(fd))ok=0;
    if(ok&&rename(temporary,path)==0)return 0;
    int error=errno;unlink(temporary);errno=error?error:EIO;return -1;
}
static inline int ble_relay_save(int enabled){
    if(ble_relay_enabled()==!!enabled)return 0;
    char value=enabled?'1':'0';
    return ble_save_config(BLE_RELAY_CONFIG,BLE_RELAY_CONFIG ".tmp",&value,1);
}
static inline void ble_saved_peer(struct BlePeer *peer){
    struct {uint32_t magic;struct BlePeer peer;} config;
    int fd=open(BLE_CONFIG,O_RDONLY|O_CLOEXEC);if(fd<0)return;
    if(read(fd,&config,sizeof(config))==sizeof(config)&&config.magic==BLE_DEVICE_MAGIC&&
       config.peer.type>=1&&config.peer.type<=2){*peer=config.peer;peer->name[63]=0;}
    close(fd);
}
static inline int ble_same_peer(const struct BlePeer *a,const struct BlePeer *b){
    return a->type&&a->type==b->type&&!memcmp(a->address,b->address,sizeof(a->address));
}
/* Store only the explicitly enabled HUD's address type and six address bytes. */
static inline int ble_expanded_icons(const struct BlePeer *peer){
    if(peer->type<1||peer->type>2)return 0;
    int fd=open(BLE_ICON_CONFIG,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;
    unsigned char record[8];
    ssize_t n=read(fd,record,sizeof(record));close(fd);
    return n==7&&record[0]==peer->type&&!memcmp(record+1,peer->address,sizeof(peer->address));
}
static inline int ble_save_icon_mode(const struct BlePeer *peer,int enabled){
    if(ble_expanded_icons(peer)==!!enabled)return 0;
    unsigned char record[7]={0};
    if(enabled){record[0]=peer->type;memcpy(record+1,peer->address,sizeof(peer->address));}
    return ble_save_config(BLE_ICON_CONFIG,BLE_ICON_CONFIG ".tmp",record,sizeof(record));
}
static inline void nav_transport_dump(void){
    int fd=open(NAV_PATH,O_RDONLY|O_CLOEXEC);if(fd<0){puts("\n[nav_transport]\navailable=0\n[end_nav_transport]");return;}
    if(lseek(fd,0,SEEK_END)!=NAV_BYTES){close(fd);puts("\n[nav_transport]\ninvalid_size=1\n[end_nav_transport]");return;}
    const struct NavRing *r=mmap(NULL,NAV_BYTES,PROT_READ,MAP_SHARED,fd,0);close(fd);
    if(r==MAP_FAILED)return;struct NavState s;
    if(r->magic==NAV_MAGIC&&nav_state(r,&s))printf("\n[nav_transport]\nproducer_pid=%u\nactive=%u\nepoch=%u\nwrite_sequence=%u\nproducer_dropped=%u\noversize=%u\nslots=%u\nram_bytes=%u\n[end_nav_transport]\n",
        s.pid,s.active,s.epoch,s.write_seq,s.dropped,s.oversize,NAV_SLOTS,NAV_BYTES);
    else puts("\n[nav_transport]\nsnapshot_unavailable=1\n[end_nav_transport]");
    munmap((void *)r,NAV_BYTES);
}

static inline void ble_link_event_dump(const struct BleLinkEvent *e) {
    const int32_t *v = e->values;
    static const char *const io_names[] = {"wait", "poll", "send", "recv", "connect", "write_response"};
    static const char *const monitor_names[] = {"listening", "socket", "filter", "bind", "read"};
    static const char *const transports[] = {"br_edr", "le_public", "le_random"};
    printf("[%u.%03u] attempt=%u ", e->seconds, e->milliseconds, e->attempt);
    switch (e->kind) {
        case BL_ATTEMPT: fputs("event=connect_attempt", stdout); break;
        case BL_HCI_MONITOR:
            printf("event=hci_monitor operation=%s error=%d", (unsigned)v[0] < 5 ? monitor_names[v[0]] : "unknown", v[1]);
            break;
        case BL_HCI_HANDLE: printf("event=hci_handle handle=0x%04x", (unsigned)v[0]); break;
        case BL_READY: fputs("event=att_ready", stdout); break;
        case BL_IO_ERROR:
            printf("event=att_io_error operation=%s error=%d result=%d", (unsigned)v[0] < 6 ? io_names[v[0]] : "unknown", v[1], v[2]);
            break;
        case BL_POLL_ERROR:
            printf("event=att_poll_error revents=0x%x", (unsigned)v[0]);
            if (v[1] >= 0) printf(" so_error=%d", v[1]);
            else printf(" so_error_unavailable=%d", -v[1]);
            break;
        case BL_LOCAL_CLOSE:
            printf("event=local_att_close ready=%d", v[0]);
            if (v[1] >= 0) printf(" hci_handle=0x%04x", (unsigned)v[1]);
            else fputs(" hci_handle=unknown", stdout);
            break;
        case BL_HCI_DISCONNECT:
            printf("event=hci_disconnect handle=0x%04x hci_reason=0x%02x observed_after_local_close=%d",
                   (unsigned)v[0], (unsigned)v[1], v[2]);
            break;
        case BL_MGMT_DISCONNECT:
            fputs("event=mgmt_disconnect", stdout);
            if (v[0] >= 0) printf(" mgmt_reason=0x%02x", (unsigned)v[0]);
            else fputs(" mgmt_reason=unavailable", stdout);
            if (v[1] >= 0) printf(" observed_after_local_close=%d", v[1]);
            else fputs(" observed_after_local_close=unknown", stdout);
            break;
        case BL_FIRST_WRITE: printf("event=first_att_write bytes=%d write_response=%d", v[0], v[1]); break;
        case BL_RETRY:
            printf("event=retry_scheduled error=%d delay_ms=%d retry_kind=%s", v[0], v[1], v[2] ? "fast" : "normal");
            break;
        case BL_HCI_PARAMS:
        case BL_HCI_PARAMS_UPDATED:
            printf("event=%s conn_interval_us=%u peripheral_latency=%u supervision_timeout_ms=%u",
                   e->kind==BL_HCI_PARAMS ? "le_connection_parameters" : "le_connection_parameters_updated",
                   (unsigned)v[0]*1250, (unsigned)v[1], (unsigned)v[2]*10);
            break;
        case BL_HCI_PARAMS_REJECTED:
            printf("event=le_connection_parameters_rejected handle=0x%04x status=0x%02x", (unsigned)v[0], (unsigned)v[1]);
            break;
        case BL_OTHER_PEER:
            printf("event=other_peer_%s transport=%s", v[1] ? "connected" : "disconnected",
                   (unsigned)v[0]<3 ? transports[v[0]] : "unknown");
            if (!v[1] && v[2]>=0) printf(" mgmt_reason=0x%02x", (unsigned)v[2]);
            break;
        case BL_FIRST_NAVIGATION:
            printf("event=first_navigation_write icon=%u distance_m=%u write_response=%d",
                   (unsigned)v[0], (unsigned)v[1], v[2]);
            break;
        case BL_HCI_HARDWARE_ERROR:
            printf("event=hci_hardware_error code=0x%02x", (unsigned)v[0]);
            break;
        default: fputs("event=unknown", stdout); break;
    }
    if (e->last_write_age_ms != UINT32_MAX) printf(" last_write_age_ms=%u", e->last_write_age_ms);
    if (e->last_navigation_write_age_ms != UINT32_MAX)
        printf(" last_navigation_write_age_ms=%u", e->last_navigation_write_age_ms);
    putchar('\n');
}
static inline void ble_dump(void) {
    nav_transport_dump();
    struct utsname kernel;if(uname(&kernel)==0)printf("\nbluetooth_kernel_release=%s\nbluetooth_kernel_machine=%s\n",kernel.release,kernel.machine);
    struct BleShared *s=ble_map(0,NULL);if(!s){puts("\n[hud_bluetooth]\navailable=0\n[end_hud_bluetooth]");return;}
    struct BleSnapshot v;
    if(!ble_snapshot(s,&v)){munmap(s,BLE_BYTES);puts("\n[hud_bluetooth]\nsnapshot=busy\n[end_hud_bluetooth]");return;}
    char address[18];ble_address(v.selected.address,address);
    printf("\n[hud_bluetooth]\nversion=R18\nworker_pid=%u\nworker_alive=%u\nphase=%u\nbusy=%u\nconnected=%u\n"
           "stage=%s\nerror=%u\nmgmt_status=%u\natt_error=%u\ncontroller_known=%u\ncontroller_index=%u\n"
           "controller_settings=0x%08x\nclassic_connections=%u\ntarget_address=%s\ntarget_type=%u\n"
           "write_uuid=0x%04x\nwrite_handle=0x%04x\nwrite_properties=0x%02x\ntest_count=%u\nclear_sent=%u\n"
           "commands_sent=%u\ndisplay_confirmation=not_available\n",
           s->worker_pid,ble_alive(s),v.phase,v.busy,v.connected,v.stage,v.error,v.mgmt_status,v.att_error,
           v.controller_known,v.controller_index,v.controller_settings,v.classic_connections,address,v.selected.type,
           v.write_uuid,v.write_handle,v.write_properties,v.test_count,v.clear_sent,v.commands_sent);
    printf("target_name=");ble_json_string(v.selected.name);putchar('\n');
    printf("last_test=%u\n",v.last_test);
    printf("expanded_icons=%u\n",(unsigned)ble_expanded_icons(&v.selected));
    printf("relay_enabled=%u\nrelay_input_count=%u\nrelay_bad=%u\nrelay_gaps=%u\nrelay_stale=%u\n"
           "relay_epoch=%u\nrelay_sequence=%u\nrelay_route_state=%u\nrelay_distance_m=%u\n"
           "relay_frames=%u\nrelay_chunks=%u\nrelay_last_tx_seconds=%u\nrelay_last_command=0x%02x\nrelay_icon=%u\n"
           "relay_core_phase=%u\nrelay_retries=%u\nrelay_retry_seconds=%u\nrelay_clear_sent=%u\n",
           v.relay_enabled,v.relay_input_count,v.relay_bad,v.relay_gaps,v.relay_stale,
           v.relay_epoch,v.relay_sequence,v.relay_route_state,v.relay_distance,
           v.relay_frames,v.relay_chunks,v.relay_last_tx,v.relay_last_command,v.relay_icon,
           v.relay_core_phase,v.relay_retries,v.relay_retry_seconds,v.relay_clear_sent);
    printf("relay_source=");ble_json_string(v.relay_source);putchar('\n');
    printf("relay_keepalives=%u\nrelay_keepalive_last_seconds=%u\nrelay_input_last_seconds=%u\n"
           "relay_raw_index=%u\nrelay_selected_index=%u\nrelay_selection_reason=first_advertised\nrelay_freshness_expired=%u\n"
           "relay_raw_type=%u\nrelay_selected_type=%u\n",
           v.relay_keepalives,v.relay_keepalive_last_seconds,v.relay_input_last_seconds,
           v.relay_raw_index,v.relay_selected_index,
           v.relay_freshness_expired,v.relay_raw_type,v.relay_selected_type);
    unsigned count=v.clear_seen==s->clear_request?v.event_count:0;if(count>BLE_EVENTS)count=BLE_EVENTS;
    printf("event_count=%u\nevent_capacity=%u\nevents_overwritten=%u\n",count,BLE_EVENTS,
           count&&v.event_next>BLE_EVENTS?v.event_next-BLE_EVENTS:0);
    for(unsigned i=0;i<count;i++){
        const struct BleEvent *e=&v.events[(v.event_next+BLE_EVENTS-count+i)%BLE_EVENTS];
        printf("[%u] stage=%s error=%u detail=",e->seconds,e->stage,e->error);ble_json_string(e->detail);putchar('\n');
    }
    puts("[end_hud_bluetooth]");
    count = v.clear_seen == s->clear_request ? v.link_event_count : 0;
    if (count > BLE_LINK_EVENTS) count = BLE_LINK_EVENTS;
    printf("\n[hud_bluetooth_link]\nevent_count=%u\nevent_capacity=%u\nevents_overwritten=%u\n",
           count, BLE_LINK_EVENTS, count && v.link_event_next > BLE_LINK_EVENTS ? v.link_event_next - BLE_LINK_EVENTS : 0);
    for (unsigned i = 0; i < count; ++i)
        ble_link_event_dump(&v.link_events[(v.link_event_next + BLE_LINK_EVENTS - count + i) % BLE_LINK_EVENTS]);
    puts("[end_hud_bluetooth_link]");
    munmap(s,BLE_BYTES);
}
#endif
