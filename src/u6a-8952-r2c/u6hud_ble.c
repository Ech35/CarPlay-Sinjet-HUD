#define _POSIX_C_SOURCE 200809L
#include "ble_shared.h"
#include "u6diag.h"
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "../ble-r18/core/hud_core.h"
#include "../ble-r18/core_adapter.h"
#include "../ble-r18/core/vendor/rgd/rgd_tlv.h"
#include "../hud-icons-r3/icon_ids.h"
#include "nav_ipc.h"
#include "carplay_gate.h"

struct HciAddress { unsigned short family,device,channel; };
struct L2Address { unsigned short family,psm; unsigned char address[6]; unsigned short cid; unsigned char type; };
_Static_assert(sizeof(struct HciAddress)==6&&sizeof(struct L2Address)==14,"Bluetooth ABI mismatch");
static struct BleShared *shared;
static struct BleSnapshot view;
static struct Diag *iap;
static int mgmt=-1,att=-1;
static unsigned char controller_address[6];
static volatile sig_atomic_t terminated;
static int scan_owned;
static int cached_gate;
static unsigned cached_session_pid;
static void relay_ingest(void);
static int relay_in_write;
static int64_t relay_write_deadline,connection_deadline;
static unsigned worker_self_pid;
static uint64_t worker_self_ticks;
static struct CarplayGate carplay_gate;
static const char *main_state_reason="state_unavailable";
static int main_state_errno;
static const unsigned char start_frame[]={0xff,0x55,3,1,0xe0,1,0xe5};
static const unsigned char clear_frame[]={0xff,0x55,3,1,0xe0,0,0xe4};
static unsigned le16(const unsigned char *p){return p[0]|p[1]<<8;}
static unsigned le32(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
static void put16(unsigned char *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void copy_text(char *out,unsigned cap,const char *text){snprintf(out,cap,"%s",text?text:"");}
static int main_state(void){
    if(iap)return 1;
    int fd=open(DIAG_PATH,O_RDONLY|O_CLOEXEC);if(fd<0){main_state_reason="state_open_failed";main_state_errno=errno;return 0;}
    off_t size=lseek(fd,0,SEEK_END);
    if(size!=(off_t)sizeof(*iap)){main_state_reason="state_size_invalid";main_state_errno=size<0?errno:EINVAL;close(fd);return 0;}
    void *p=mmap(NULL,sizeof(*iap),PROT_READ,MAP_SHARED,fd,0);close(fd);
    if(p==MAP_FAILED){main_state_reason="state_map_failed";main_state_errno=errno;return 0;}
    if(((struct Diag *)p)->magic!=DIAG_MAGIC){main_state_reason="state_magic_invalid";main_state_errno=EINVAL;munmap(p,sizeof(*iap));return 0;}
    iap=p;return 1;
}
static int carplay_ready(void){
#ifdef U5BLE_PEER_TEST
    extern int peer_carplay;cached_session_pid=peer_carplay?123:0;return peer_carplay;
#else
    cached_gate=0;cached_session_pid=0;
    if(!main_state()){
        memset(&carplay_gate,0,sizeof(carplay_gate));carplay_gate.reason=main_state_reason;
        carplay_gate.error=main_state_errno;return 0;
    }
    cached_gate=u6_carplay_gate(iap,&carplay_gate);
    cached_session_pid=cached_gate?carplay_gate.pid:0;
    return cached_gate;
#endif
}
static int logging_enabled(void){
#ifdef U5BLE_PEER_TEST
    extern int peer_logging;
    return peer_logging;
#else
    return main_state()&&__atomic_load_n(&iap->enabled,__ATOMIC_ACQUIRE);
#endif
}
#include "link_diag.h"

static void publish(void){
    if(!shared)return;
    if(worker_self_pid){
        shared->start_low=(unsigned)worker_self_ticks;shared->start_high=(unsigned)(worker_self_ticks>>32);
        __atomic_store_n(&shared->worker_pid,worker_self_pid,__ATOMIC_RELEASE);
    }
    if (view.clear_seen != shared->clear_request) {
        view.clear_seen = shared->clear_request;
        view.event_count = view.event_next = view.link_event_count = view.link_event_next = 0;
        memset(view.events, 0, sizeof(view.events));
        memset(view.link_events, 0, sizeof(view.link_events));
    }
    __atomic_store_n(&shared->worker_heartbeat,(unsigned)(ble_now()/1000),__ATOMIC_RELEASE);
    __atomic_add_fetch(&shared->snapshot_seq,1,__ATOMIC_ACQ_REL);
    memcpy(&shared->snapshot,&view,sizeof(view));
    __atomic_add_fetch(&shared->snapshot_seq,1,__ATOMIC_RELEASE);
}
static void event(const char *stage,unsigned error,const char *format,...){
    copy_text(view.stage,sizeof(view.stage),stage);view.error=error;
    if(logging_enabled()){
        struct BleEvent *e=&view.events[view.event_next++%BLE_EVENTS];
        memset(e,0,sizeof(*e));e->seconds=(unsigned)(ble_now()/1000);e->error=error;copy_text(e->stage,sizeof(e->stage),stage);
        va_list ap;va_start(ap,format);vsnprintf(e->detail,sizeof(e->detail),format,ap);va_end(ap);
        if(view.event_count<BLE_EVENTS)view.event_count++;
    }publish();
}
static int cancelled(void){
    if(shared)__atomic_store_n(&shared->worker_heartbeat,(unsigned)(ble_now()/1000),__ATOMIC_RELEASE);
    if(terminated||(shared&&__atomic_load_n(&shared->cancel,__ATOMIC_ACQUIRE))){errno=ECANCELED;return 1;}
    if(!carplay_ready()){errno=EHOSTDOWN;return 1;}return 0;
}
static int wait_io(int fd,short events,int64_t deadline,int cleanup){
    if(connection_deadline&&connection_deadline<deadline)deadline=connection_deadline;
    for(;;){
        if(!cleanup&&!relay_in_write)relay_ingest();
        if(!cleanup&&cancelled())return -1;
        int64_t left=deadline-ble_now();
        if (left <= 0) {
            if (fd == att) link_io_error(BL_WAIT, ETIMEDOUT, events);
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd p={.fd=fd,.events=events};int n=poll(&p,1,left>100?100:(int)left);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (fd == att) link_io_error(BL_POLL, errno, n);
            return -1;
        }
        if (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            if (fd == att) link_poll_error(p.revents);
            errno = ECONNRESET;
            return -1;
        }
        if (n > 0 && (p.revents & events)) return 0;
    }
}
static int send_bytes(int fd,const void *data,unsigned bytes,int64_t deadline,int cleanup){
    for(;;){if(wait_io(fd,POLLOUT,deadline,cleanup))return -1;
        ssize_t n=send(fd,data,bytes,MSG_NOSIGNAL);if(n==(ssize_t)bytes)return 0;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (fd == att) link_io_error(BL_SEND, n < 0 ? errno : 0, (int)n);
        if (n >= 0) errno = EIO;
        return -1;
    }
}
static void found_device(const unsigned char *p,unsigned n){
    if(n<14||p[6]<1||p[6]>2||le16(p+12)!=n-14)return;
    unsigned slot=view.peer_count;
    for(unsigned i=0;i<view.peer_count;i++)if(view.peers[i].type==p[6]&&!memcmp(view.peers[i].address,p,6)){slot=i;break;}
    if(slot>=BLE_PEERS)return;
    if(slot==view.peer_count){memset(&view.peers[slot],0,sizeof(view.peers[slot]));view.peer_count++;}
    struct BlePeer *peer=&view.peers[slot];memcpy(peer->address,p,6);peer->type=p[6];peer->rssi=(signed char)p[7];
    peer->connectable=!(le32(p+8)&4);
    for(unsigned off=14;off<n;){unsigned len=p[off];if(!len)break;if(len>n-off-1)break;
        unsigned type=p[off+1];if((type==8||type==9)&&len>1){
            unsigned count=len-1;if(count>=sizeof(peer->name))count=sizeof(peer->name)-1;
            memcpy(peer->name,p+off+2,count);peer->name[count]=0;
        }off+=len+1;
    }publish();
}
static void observe_mgmt(unsigned code,unsigned index,const unsigned char *p,unsigned n){
    link_mgmt_event(code, index, p, n);
    if(index!=view.controller_index)return;
    if(code==0x12&&view.phase==BS_SCANNING)found_device(p,n);
    if(code==0x0b&&n>=7&&p[6]==0){view.classic_connections++;publish();}
}
static int read_mgmt(unsigned char *packet,unsigned cap,int64_t deadline,int cleanup){
    for(;;){if(wait_io(mgmt,POLLIN,deadline,cleanup))return -1;
        ssize_t n=recv(mgmt,packet,cap,MSG_TRUNC);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(n<6||n>(ssize_t)cap||le16(packet+4)!=(unsigned)n-6){errno=EPROTO;return -1;}
        observe_mgmt(le16(packet),le16(packet+2),packet+6,(unsigned)n-6);return (int)n;
    }
}
static int command(unsigned opcode,unsigned index,const unsigned char *p,unsigned n,unsigned char *reply,unsigned cap,int cleanup){
    unsigned char request[32];if(n>sizeof(request)-6){errno=EMSGSIZE;return -1;}
    put16(request,opcode);put16(request+2,index);put16(request+4,n);if(n)memcpy(request+6,p,n);
    int64_t deadline=ble_now()+2500;
    if(send_bytes(mgmt,request,n+6,deadline,cleanup))return -1;
    for(;;){unsigned char packet[4096];int bytes=read_mgmt(packet,sizeof(packet),deadline,cleanup);if(bytes<0)return -1;
        unsigned code=le16(packet),which=le16(packet+2);
        if((code!=1&&code!=2)||which!=index||bytes<9||le16(packet+6)!=opcode)continue;
        view.mgmt_status=packet[8];
        if(packet[8]){errno=packet[8]==0x0a?EBUSY:packet[8]==0x0f?ENETDOWN:packet[8]==0x0c?EOPNOTSUPP:EIO;return -1;}
        if(code==2)continue;
        unsigned size=(unsigned)bytes-9;if(size>cap){errno=EMSGSIZE;return -1;}
        if(size)memcpy(reply,packet+9,size);return (int)size;
    }
}
static int open_controller(void){
    if(mgmt<0){
        mgmt=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,1);
        if(mgmt<0){event("controller_socket",errno,"kernel management socket unavailable");return -1;}
        struct HciAddress a={AF_BLUETOOTH,0xffff,3};
        if(bind(mgmt,(struct sockaddr *)&a,sizeof(a))){int e=errno;close(mgmt);mgmt=-1;event("controller_bind",e,"management channel unavailable");errno=e;return -1;}
    }
    unsigned char reply[512];int n=command(3,0xffff,NULL,0,reply,sizeof(reply),0);
    if(n<2||le16(reply)!=1||n!=4){if(n>=0)errno=n<2?EPROTO:le16(reply)?EEXIST:ENODEV;event("controller_list",errno,"expected one controller");return -1;}
    view.controller_index=le16(reply+2);view.controller_known=1;
    n=command(4,view.controller_index,NULL,0,reply,sizeof(reply),0);
    if(n<17){if(n>=0)errno=EPROTO;event("controller_info",errno,"read information failed");return -1;}
    memcpy(controller_address,reply,6);unsigned supported=le32(reply+9);view.controller_settings=le32(reply+13);publish();
    if(!(supported&(1u<<9))){errno=EOPNOTSUPP;event("le_unsupported",errno,"controller reports no LE support");return -1;}
    if(!(view.controller_settings&1)){errno=ENETDOWN;event("controller_off",errno,"native adapter not powered");return -1;}
    if(!(view.controller_settings&(1u<<9))){errno=EOPNOTSUPP;event("le_disabled",errno,"native LE setting disabled; not changed");return -1;}
    return 0;
}
static int check_classic(void){
    unsigned char reply[512];int n=command(0x15,view.controller_index,NULL,0,reply,sizeof(reply),0);
    if(n<2||n!=2+(int)le16(reply)*7){if(n>=0)errno=EPROTO;event("connection_list",errno,"cannot verify phone pairing channel");return -1;}
    view.classic_connections=0;
    for(unsigned off=2;off<(unsigned)n;off+=7)if(reply[off+6]==0)view.classic_connections++;
    publish();if(view.classic_connections){errno=EBUSY;event("phone_bluetooth_busy",errno,"classic connections=%u",view.classic_connections);return -1;}return 0;
}
static int prerequisites(void){
    if(cancelled()){event("carplay_or_cancel",errno,"CarPlay must be active; cancelled=%u",shared?shared->cancel:0);return -1;}
    return open_controller()||check_classic()?-1:0;
}
static int scan_devices(void){
    if(prerequisites())return -1;
    view.phase=BS_SCANNING;view.peer_count=0;memset(view.peers,0,sizeof(view.peers));event("scan_start",0,"LE discovery, six seconds");
    const unsigned char type=6;unsigned char response[16];
    /* Finish the bounded start acknowledgement even when cancel arrives in
     * flight, so an acknowledged scan is always paired with our own stop. */
    if(command(0x23,view.controller_index,&type,1,response,sizeof(response),1)<0){event("scan_start_failed",errno,"native discovery is not stopped without a start acknowledgement");return -1;}
    scan_owned=1;int64_t deadline=ble_now()+6000;int failure=0;
    while(ble_now()<deadline){unsigned char packet[4096];int n=read_mgmt(packet,sizeof(packet),deadline,0);
        if(n<0){if(errno!=ETIMEDOUT)failure=errno;break;}
        if(view.classic_connections){failure=EBUSY;break;}
    }
    /* This stop is issued only after our own start was acknowledged. */
    int stopped=command(0x24,view.controller_index,&type,1,response,sizeof(response),1);
    if(stopped>=0)scan_owned=0;
    else if(!failure)failure=errno;
    if(failure){errno=failure;event("scan_stopped",failure,"devices=%u stop_ack=%d",view.peer_count,stopped>=0);return -1;}
    event("scan_complete",0,"devices=%u",view.peer_count);return 0;
}
static int receive_att(unsigned char *p,unsigned cap,int64_t deadline,int cleanup){
    for(;;){if(wait_io(att,POLLIN,deadline,cleanup))return -1;
        ssize_t n=recv(att,p,cap,MSG_TRUNC);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;
        if (n <= 0 || n > (ssize_t)cap) {
            link_io_error(BL_RECV, n < 0 ? errno : 0, (int)n);
            if (n >= 0) errno = n ? EMSGSIZE : ECONNRESET;
            return -1;
        }
        if(p[0]==0x1b)continue;
        if(p[0]==0x1d){const unsigned char ack=0x1e;if(send_bytes(att,&ack,1,deadline,cleanup))return -1;continue;}
        return (int)n;
    }
}
static unsigned short_uuid(const unsigned char *p,unsigned n){
    const unsigned char base[]={0xfb,0x34,0x9b,0x5f,0x80,0,0,0x80,0,0x10,0,0};
    if(n==2)return le16(p);
    return n==16&&!p[14]&&!p[15]&&!memcmp(p,base,12)?le16(p+12):0;
}
static int discover(void){
    const unsigned ids[]={0xff02,0xffe2,0xffe1};unsigned handles[3]={0},props[3]={0},counts[3]={0};
    int64_t deadline=ble_now()+8000;
    for(unsigned first=1;first<=65535;){
        unsigned char request[]={8,(unsigned char)first,(unsigned char)(first>>8),0xff,0xff,3,0x28},response[23];
        if(send_bytes(att,request,sizeof(request),deadline,0))return -1;
        int n=receive_att(response,sizeof(response),deadline,0);if(n<0)return -1;
        if(n==5&&response[0]==1&&response[1]==8){view.att_error=response[4];if(response[4]==0x0a)break;errno=EACCES;return -1;}
        unsigned width=n>=2?response[1]:0;
        if(n<2||response[0]!=9||(width!=7&&width!=21)||(unsigned)(n-2)<width||(n-2)%width){errno=EPROTO;return -1;}
        for(unsigned off=2;off<(unsigned)n;off+=width){
            unsigned declaration=le16(response+off),handle=le16(response+off+3),properties=response[off+2];
            if(declaration<first||handle<=declaration){errno=EPROTO;return -1;}first=declaration+1;
            unsigned uuid=short_uuid(response+off+5,width-5);
            for(unsigned i=0;i<3;i++)if(uuid==ids[i]&&(properties&12)){counts[i]++;handles[i]=handle;props[i]=properties;}
        }
    }
    for(unsigned i=0;i<3;i++)if(counts[i]){
        if(counts[i]!=1){errno=EEXIST;return -1;}
        view.write_uuid=ids[i];view.write_handle=handles[i];view.write_properties=props[i];view.att_error=0;publish();return 0;
    }errno=ENOENT;return -1;
}
static void save_selected(void){
    struct {uint32_t magic;struct BlePeer peer;} config={BLE_DEVICE_MAGIC,view.selected};
    struct {uint32_t magic;struct BlePeer peer;} old;
    int previous=open(BLE_CONFIG,O_RDONLY|O_CLOEXEC);
    if(previous>=0){int same=read(previous,&old,sizeof(old))==sizeof(old)&&old.magic==BLE_DEVICE_MAGIC&&old.peer.type==view.selected.type&&
        !memcmp(old.peer.address,view.selected.address,6)&&!memcmp(old.peer.name,view.selected.name,sizeof(old.peer.name));close(previous);if(same)return;}
    const char *temporary=BLE_CONFIG ".tmp";
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return;
    int ok=write(fd,&config,sizeof(config))==sizeof(config)&&fsync(fd)==0;
    if(close(fd))ok=0;if(ok&&rename(temporary,BLE_CONFIG)==0)return;unlink(temporary);
}
static int connect_hud(const struct BlePeer *peer){
    if(prerequisites())return -1;
    view.selected=*peer;view.phase=BS_CONNECTING;view.att_error=0;
    link_begin(peer);
    event("connect_start",0,"explicit user-selected target");
    att=socket(AF_BLUETOOTH,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(att<0){event("att_socket",errno,"kernel L2CAP unavailable");return -1;}
    struct L2Address local={.family=AF_BLUETOOTH,.cid=4,.type=1};memcpy(local.address,controller_address,6);
    struct L2Address remote={.family=AF_BLUETOOTH,.cid=4,.type=peer->type};memcpy(remote.address,peer->address,6);
    if(bind(att,(struct sockaddr *)&local,sizeof(local))){event("att_bind",errno,"bind existing controller");return -1;}
    if(connect(att,(struct sockaddr *)&remote,sizeof(remote))){
        if(errno!=EINPROGRESS&&errno!=EAGAIN){link_io_error(BL_CONNECT, errno, -1);event("att_connect",errno,"target connection failed");return -1;}
        if(wait_io(att,POLLOUT,ble_now()+8000,0)){event("att_connect_wait",errno,"target did not become ready");return -1;}
        int error=0;socklen_t n=sizeof(error);
        if(getsockopt(att,SOL_SOCKET,SO_ERROR,&error,&n)||error){if(error)errno=error;link_io_error(BL_CONNECT, errno, -1);event("att_connect_result",errno,"target rejected or disconnected");return -1;}
    }
    link_watch.had_link = 1;
    view.phase=BS_DISCOVERING;event("discover_start",0,"read writable characteristic declarations");
    if(discover()){event("discover_failed",errno,"allowed UUID missing, ambiguous or ATT rejected");return -1;}
    link_poll_events();
    link_note(link_watch.attempt, BL_READY, 0, 0, 0);
    view.connected=1;view.phase=BS_READY;save_selected();event("connected",0,"uuid=%04x handle=%04x properties=%02x",view.write_uuid,view.write_handle,view.write_properties);return 0;
}
static int write_frame(const unsigned char *p,unsigned n,int cleanup){
    if(att<0||n>20){errno=ENOTCONN;return -1;}
    unsigned char packet[23];int response=!!(view.write_properties&8);
    packet[0]=response?0x12:0x52;put16(packet+1,view.write_handle);memcpy(packet+3,p,n);
    int64_t deadline=ble_now()+1500;
    if(relay_write_deadline&&deadline>relay_write_deadline)deadline=relay_write_deadline;
    if(send_bytes(att,packet,n+3,deadline,cleanup))return -1;
    view.commands_sent++;publish();
    if (response) {
        /* Once a test write is sent, finish its ATT response before cleanup.
         * Cancellation still stops the next test frame or the display timer. */
        int count = receive_att(packet, sizeof(packet), deadline, cleanup||view.phase==BS_TESTING);
        if (count < 0) return -1;
        if (count != 1 || packet[0] != 0x13) {
            if (count == 5 && packet[0] == 1) view.att_error = packet[4];
            link_io_error(BL_WRITE_RESPONSE, EPROTO, count == 5 && packet[0] == 1 ? packet[4] : 0);
            errno = EPROTO;
            return -1;
        }
    }
    link_written(n, response, cleanup);
    return 0;
}
static void close_hud(int clear){
    if(att>=0){if(clear&&view.connected){
            view.clear_sent=write_frame(clear_frame,sizeof(clear_frame),1)==0;
            /* A command has no ATT response to wait for. Give the queued
             * clear a short send window before closing our own HUD socket. */
        }
        if(view.connected&&!(view.write_properties&8)&&(view.clear_sent||view.relay_clear_sent)){
            struct timespec pause={.tv_nsec=150000000};
            while(nanosleep(&pause,&pause)&&errno==EINTR){}
        }
        link_poll_events();
        link_note(link_watch.attempt, BL_LOCAL_CLOSE, view.connected, link_watch.handle, 0);
        link_watch.closed = 1;
        close(att);att=-1;}
    view.connected=0;publish();
}
static int write_test_payload(const uint8_t *payload,size_t length,int cleanup){
    uint8_t wire[20];
    size_t size=u5_core_encode_frame(wire,sizeof(wire),payload,length);
    if(!size){errno=EMSGSIZE;return -1;}
    struct timespec pause={.tv_nsec=16000000};
    while(nanosleep(&pause,&pause)&&errno==EINTR){}
    return write_frame(wire,(unsigned)size,cleanup);
}
static int test_lanes(int clear){
    /* Six lanes from left to right. Lanes 2 and 3 recommend the left turn.
     * Each detail pair is the background arrow followed by its highlight. */
    const uint8_t simple[]={0x11,clear?0:6,0,1,1,0,0,0};
    const uint8_t v1[]={0x12,1,15,1,1,2,1,0,15,4,15,3,15};
    uint8_t v2[sizeof(v1)];memcpy(v2,v1,sizeof(v1));v2[0]=0x15;
    const struct {const uint8_t *payload;size_t length;} frames[]={
        {simple,clear?2:sizeof(simple)}, {v1,clear?1:sizeof(v1)}, {v2,clear?1:sizeof(v2)},
    };
    int error=0;
    for(unsigned i=0;i<sizeof(frames)/sizeof(frames[0]);i++){
        if(!write_test_payload(frames[i].payload,frames[i].length,clear))continue;
        if(!error)error=errno;
        if(!clear)break;
    }
    return error;
}
static int test_hud(unsigned sample){
    if(att<0||!view.connected){errno=ENOTCONN;event("test_not_connected",errno,"connect the selected HUD first");return -1;}
    if(prerequisites())return -1;
    unsigned icon=2;
    if(sample>BLE_TEST_LANES){
        if(sample<HUD_ICON_FOLLOW_ROAD||sample>HUD_ICON_EXIT_19||!ble_expanded_icons(&view.selected)){
            errno=EPERM;event("test_icons_disabled",errno,"expanded icons require the saved HUD setting");return -1;
        }
        icon=sample;
    }
    const uint8_t turn[]={0,(uint8_t)icon,100,0,0,0};
    view.phase=BS_TESTING;view.clear_sent=0;
    event("test_start",0,"sample=%u icon=%u distance=100 lanes=%u",sample,icon,sample==BLE_TEST_LANES?6:0);
    int error=write_frame(start_frame,sizeof(start_frame),0)?errno:0;
    if(!error&&write_test_payload(turn,sizeof(turn),0))error=errno;
    if(!error&&sample==BLE_TEST_LANES)error=test_lanes(0);
    if(!error){
        view.last_test=sample;view.test_count++;view.last_test_seconds=(unsigned)(ble_now()/1000);
        event("test_written",0,"sample=%u; verify the actual HUD; write response=%u",sample,!!(view.write_properties&8));
        int64_t until=ble_now()+3000;
        while(ble_now()<until){if(cancelled()){error=errno;break;}struct timespec t={.tv_nsec=50000000};nanosleep(&t,NULL);}
    }
    int clear_error=write_frame(clear_frame,sizeof(clear_frame),1)?errno:0;
    if(sample==BLE_TEST_LANES){
        int lane_error=test_lanes(1);
        if(!clear_error)clear_error=lane_error;
    }
    view.clear_sent=!clear_error;
    if(!error)error=clear_error;
    if(error){errno=error;event("test_failed",error,"clear_sent=%u",view.clear_sent);return -1;}
    view.phase=BS_READY;event("test_complete",0,"test sent and cleared; visual result unconfirmed");return 0;
}
#include "relay.h"
static void on_signal(int number){(void)number;terminated=1;}
static int worker(void){
    int lock=open(BLE_LOCK,O_RDWR|O_CREAT|O_CLOEXEC,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 1;
    shared=ble_map(1,NULL);if(!shared)return 1;
    uint64_t ticks=ble_process_start((unsigned)getpid());shared->start_low=(unsigned)ticks;shared->start_high=(unsigned)(ticks>>32);
    worker_self_pid=(unsigned)getpid();worker_self_ticks=ticks;
    __atomic_store_n(&shared->worker_pid,(unsigned)getpid(),__ATOMIC_RELEASE);
    if(!ble_snapshot(shared,&view))memset(&view,0,sizeof(view));
    view.phase=BS_IDLE;view.busy=view.connected=0;
    memset(&view.selected,0,sizeof(view.selected));view.relay_enabled=0;
    nav_ring=nav_open();nav_core=u5_core_create(0x7f01);
    if(nav_core)hc_transport_reset(nav_core,0);
    signal(SIGTERM,on_signal);signal(SIGINT,on_signal);
    event(nav_ring&&nav_core?"worker_ready":"relay_init_failed",nav_ring&&nav_core?0:ENOMEM,"RAM navigation service; radio starts only on user-selected target");
    int64_t last_gate=0,last_link_poll=0,restore_at=0;
    int restore_pending=1,restore_wait_logged=0;
    while(!terminated){
        int64_t now = ble_now();
        if (now - last_link_poll >= 100) {
            link_poll_events();
            last_link_poll = now;
        }
        publish();
        if(shared->cancel){
            view.relay_enabled=0;relay_stop_link(0);view.phase=BS_IDLE;
            __atomic_store_n(&shared->cancel,0,__ATOMIC_RELEASE);
            event("disconnected",0,"requested; navigation relay disabled");
        }
        unsigned sequence=__atomic_load_n(&shared->request_seq,__ATOMIC_ACQUIRE);
        /* UDISK can mount after this early boot worker starts. Restore once
         * CarPlay is ready, and retry if the saved target is not visible yet.
         * Explicit user commands always take priority over boot restoration. */
        if(restore_pending&&sequence==shared->handled_seq&&now>=restore_at){
            restore_at=now+1000;
            if(carplay_ready()){
                struct BlePeer saved={0};ble_saved_peer(&saved);
                if(saved.type>=1&&saved.type<=2){
                    view.selected=saved;view.relay_enabled=ble_relay_enabled();
                    relay_retry_at=0;relay_retry_count=0;restore_pending=0;
                    if(nav_core)hc_transport_reset(nav_core,view.relay_enabled&&live_nav.active);
                    event("settings_restored",0,"saved HUD loaded after CarPlay readiness; relay=%u",view.relay_enabled);
                }else if(!restore_wait_logged){
                    restore_wait_logged=1;
                    event("settings_wait",0,"saved HUD not visible yet; retrying until user setup");
                }
            }
        }
        if(sequence!=shared->handled_seq){
            restore_pending=0;
            struct BleRequest request=shared->request;view.busy=1;view.request_id=sequence;view.error=view.mgmt_status=view.att_error=0;publish();
            __atomic_store_n(&shared->handled_seq,sequence,__ATOMIC_RELEASE);
            int result=-1;
            if(request.operation==BLE_RELAY_ON){
                if(nav_core&&nav_ring){
                    view.selected=request.peer;view.relay_enabled=1;
                    hc_transport_reset(nav_core,1);relay_retry_at=0;relay_retry_count=0;
                    event("relay_enabled",0,"automatic navigation relay enabled");result=0;
                }else {errno=ENOMEM;event("relay_init_failed",errno,"navigation RAM service unavailable");}
            }else if(request.operation==BLE_RELAY_OFF){
                view.relay_enabled=0;relay_stop_link(0);event("relay_disabled",0,"navigation stopped and own HUD connection closed");result=0;
            }else if(view.relay_enabled){errno=EBUSY;event("relay_active",errno,"disable navigation relay before manual operations");}
            else if(request.operation==BLE_SCAN&&att<0)result=scan_devices();
            else if(request.operation==BLE_CONNECT&&att<0)result=connect_hud(&request.peer);
            else if(request.operation==BLE_TEST)result=test_hud(request.test);
            else {errno=EBUSY;event("operation_busy",errno,"disconnect before searching or selecting another target");}
            if(result){int failure=errno;close_hud(request.operation!=BLE_TEST);view.phase=failure==ECANCELED?BS_IDLE:BS_ERROR;view.error=failure?failure:EIO;}
            else if(att<0)view.phase=BS_IDLE;
            view.busy=0;publish();last_gate=ble_now();
        }
        relay_service();
        if(att>=0&&ble_now()-last_gate>=1000){
            last_gate=ble_now();
            if(prerequisites()){int e=errno;
                if(view.relay_enabled){relay_stop_link(!carplay_ready()||!live_nav.active);relay_retry_at=ble_now()+5000;}
                else close_hud(1);
                view.phase=BS_ERROR;event("connection_guard",e,"CarPlay stopped or phone pairing channel active");}
        }
        if(att>=0){struct pollfd p={att,POLLIN,0};int rc=poll(&p,1,16);
            if(rc>0&&(p.revents&(POLLERR|POLLHUP|POLLNVAL))){
                link_poll_error(p.revents);
                if(view.relay_enabled)relay_link_failed(ECONNRESET,"peer_disconnected");
                else {close_hud(0);view.phase=BS_ERROR;event("peer_disconnected",ECONNRESET,"HUD connection closed");}}
            else if(rc>0&&(p.revents&POLLIN)){unsigned char packet[23];int n=receive_att(packet,sizeof(packet),ble_now()+200,0);
                if(n<0&&errno!=ETIMEDOUT){int e=errno;
                    if(view.relay_enabled)relay_link_failed(e,"peer_io");
                    else {close_hud(0);view.phase=BS_ERROR;event("peer_io",e,"asynchronous receive failed");}}}
        }else {struct timespec t={.tv_nsec=100000000};nanosleep(&t,NULL);}
    }
    if(scan_owned&&mgmt>=0){unsigned char type=6,reply[16];command(0x24,view.controller_index,&type,1,reply,sizeof(reply),1);}
    relay_stop_link(1);link_hci_close();view.busy=0;view.phase=BS_IDLE;event("worker_stopped",0,"own HUD connection closed");
    if(nav_core)hc_destroy(nav_core);if(nav_ring)munmap(nav_ring,NAV_BYTES);
    worker_self_pid=0;shared->worker_pid=0;publish();if(mgmt>=0)close(mgmt);close(lock);return 0;
}

static void json_headers(void){fputs("Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n\r\n",stdout);}
static void json_error(const char *reason,int error){json_headers();printf("{\"ok\":false,\"error\":");ble_json_string(reason);printf(",\"errno\":%d}\n",error);}
static void peer_json(const struct BlePeer *p){char address[18];ble_address(p->address,address);printf("{\"address\":\"%s\",\"type\":%u,\"name\":",address,p->type);ble_json_string(p->name);printf(",\"rssi\":%d,\"connectable\":%s}",p->rssi,p->connectable?"true":"false");}
static void status(void){
    struct BleShared *s=ble_map(0,NULL);struct BleSnapshot v={0};int running=s&&ble_alive(s),ok=s&&ble_snapshot(s,&v);
    struct BlePeer saved={0};ble_saved_peer(&saved);
    if(!v.selected.type)v.selected=saved;
    int ready=carplay_ready();
    json_headers();printf("{\"ok\":true,\"version\":\"U6A_R2c\",\"running\":%s,\"carplay_ready\":%s,\"phase\":%u,"
        "\"busy\":%s,\"connected\":%s,\"stage\":",running?"true":"false",ready?"true":"false",ok?v.phase:0,
        running&&(v.busy||s->request_seq!=s->handled_seq)?"true":"false",running&&v.connected?"true":"false");
    ble_json_string(ok?v.stage:"");printf(",\"errno\":%u,\"mgmt_status\":%u,\"att_error\":%u,\"classic_connections\":%u,"
        "\"test_count\":%u,\"last_test\":%u,\"clear_sent\":%s,\"request_id\":%u,\"selected\":",v.error,v.mgmt_status,v.att_error,v.classic_connections,v.test_count,v.last_test,v.clear_sent?"true":"false",v.request_id);
    peer_json(&v.selected);
    printf(",\"icon_target_ready\":%s,\"expanded_icons\":%s",
        ble_same_peer(&saved,&v.selected)?"true":"false",ble_expanded_icons(&v.selected)?"true":"false");
    printf(",\"worker_relay_enabled\":%s,\"saved_hud_available\":%s",running&&v.relay_enabled?"true":"false",saved.type?"true":"false");
    printf(",\"relay_enabled\":%s,\"relay_input_count\":%u,\"relay_frames\":%u,\"relay_route_state\":%u,\"relay_core_phase\":%u,"
           "\"relay_icon\":%u,\"relay_distance\":%u,\"relay_bad\":%u,\"relay_gaps\":%u,\"relay_clear_sent\":%s,\"relay_source\":",
           ble_relay_enabled()?"true":"false",v.relay_input_count,v.relay_frames,v.relay_route_state,v.relay_core_phase,
           v.relay_icon,v.relay_distance,v.relay_bad,v.relay_gaps,v.relay_clear_sent?"true":"false");
    ble_json_string(v.relay_source);
    u6_carplay_gate_json(&carplay_gate);
    printf(",\"relay_keepalives\":%u,\"relay_freshness_expired\":%s,\"devices\":[",v.relay_keepalives,v.relay_freshness_expired?"true":"false");
    for(unsigned i=0;ok&&i<v.peer_count&&i<BLE_PEERS;i++){if(i)putchar(',');peer_json(&v.peers[i]);}puts("]}");
    if(s)munmap(s,BLE_BYTES);
}
static int parse_mac(const char *p,unsigned char out[6]){
    if(strlen(p)!=17)return 0;unsigned nonzero=0,not_broadcast=0;
    for(unsigned i=0;i<6;i++){
        unsigned value=0;for(unsigned j=0;j<2;j++){
            unsigned c=(unsigned char)p[i*3+j],n=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:16;
            if(n>15)return 0;value=value*16+n;}
        if(i<5&&p[i*3+2]!=':')return 0;out[5-i]=value;nonzero|=value;not_broadcast|=value^255;
    }return nonzero&&not_broadcast;
}
static int parse_test(const char *body,unsigned length,uint32_t *sample){
    if(!strcmp(body,"{}")){*sample=BLE_TEST_TURN;return 1;}
    if(!strcmp(body,"{\"lanes\":true}")){*sample=BLE_TEST_LANES;return 1;}
    if(length!=11||memcmp(body,"{\"icon\":",8)||body[10]!='}'||
       body[8]<'0'||body[8]>'9'||body[9]<'0'||body[9]>'9')return 0;
    unsigned icon=(unsigned)(body[8]-'0')*10+(unsigned)(body[9]-'0');
    if(icon<HUD_ICON_FOLLOW_ROAD||icon>HUD_ICON_EXIT_19)return 0;
    *sample=icon;return 1;
}
static void post(const char *action){
    const char *method=getenv("REQUEST_METHOD"),*type=getenv("CONTENT_TYPE"),*length=getenv("CONTENT_LENGTH");
    if(!method||strcmp(method,"POST")){json_error("post_required",EINVAL);return;}
    char *end=NULL;errno=0;unsigned long supplied=length?strtoul(length,&end,10):0;
    if(!type||strcmp(type,"application/json")||!length||!end||*end||errno||supplied<2||supplied>128){json_error("json_body_required",EINVAL);return;}
    unsigned n=(unsigned)supplied;
    char body[129];if(fread(body,1,n,stdin)!=n){json_error("body_short",EINVAL);return;}body[n]=0;
    struct BleRequest request={0};
    int icon_mode=-1;
    if(!strcmp(action,"connect")){
        /* The small public contract intentionally accepts only these fields,
         * not arbitrary JSON names, paths, UUIDs or executable arguments. */
        char address[18],kind_text=0;int consumed=0;
        if(sscanf(body,"{\"address\":\"%17[0-9A-Fa-f:]\",\"type\":%c}%n",address,&kind_text,&consumed)!=2||consumed!=(int)n||
           !parse_mac(address,request.peer.address)||kind_text<'1'||kind_text>'2'){json_error("target_invalid",EINVAL);return;}
        unsigned kind=(unsigned)(kind_text-'0');
        request.operation=BLE_CONNECT;request.peer.type=kind;request.peer.connectable=1;
    }else if(!strcmp(action,"relay")){
        if(!strcmp(body,"{\"enabled\":true}"))request.operation=BLE_RELAY_ON;
        else if(!strcmp(body,"{\"enabled\":false}"))request.operation=BLE_RELAY_OFF;
        else {json_error("relay_body_invalid",EINVAL);return;}
    }else if(!strcmp(action,"icons")){
        if(!strcmp(body,"{\"enabled\":true}"))icon_mode=1;
        else if(!strcmp(body,"{\"enabled\":false}"))icon_mode=0;
        else {json_error("icons_body_invalid",EINVAL);return;}
    }else if(!strcmp(action,"test")){
        if(!parse_test(body,n,&request.test)){json_error("test_body_invalid",EINVAL);return;}
        request.operation=BLE_TEST;
    }else if(strcmp(body,"{}")){json_error("empty_json_required",EINVAL);return;}
    else if(!strcmp(action,"scan"))request.operation=BLE_SCAN;
    else if(strcmp(action,"disconnect")){json_error("unknown_action",EINVAL);return;}
    int fd=-1;struct BleShared *s=ble_map(1,&fd);if(!s){json_error("ram_state_unavailable",errno);return;}
    if(flock(fd,LOCK_EX|LOCK_NB)){munmap(s,BLE_BYTES);close(fd);json_error("request_busy",EBUSY);return;}
    struct BleSnapshot snapshot={0};int stable=ble_snapshot(s,&snapshot),running=ble_alive(s);
    if(icon_mode>=0){
        const char *error=NULL;int failure=0;
        struct BlePeer saved={0};ble_saved_peer(&saved);
        if(running&&(!stable||snapshot.busy||s->request_seq!=s->handled_seq)){
            error="operation_busy";failure=EBUSY;
        }else if(!saved.type||(snapshot.selected.type&&!ble_same_peer(&snapshot.selected,&saved))){
            error="target_not_selected";failure=ENOENT;
        }else if(ble_save_icon_mode(&saved,icon_mode)){
            error="settings_write_failed";failure=errno;
        }
        flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);
        if(error)json_error(error,failure);
        else {json_headers();printf("{\"ok\":true,\"expanded_icons\":%s}\n",icon_mode?"true":"false");}
        return;
    }
    if(!strcmp(action,"disconnect")){
        if(ble_relay_save(0)){flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("settings_write_failed",errno);return;}
        __atomic_store_n(&s->cancel,1,__ATOMIC_RELEASE);
    }else if(request.operation!=BLE_RELAY_OFF&&((running&&(snapshot.busy||s->request_seq!=s->handled_seq||
                      (snapshot.connected&&(request.operation==BLE_SCAN||request.operation==BLE_CONNECT))))||
             (!running&&snapshot.busy&&ble_now()/1000-s->worker_heartbeat<5))){
        flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("operation_busy",EBUSY);return;
    }else if(request.operation!=BLE_RELAY_ON&&request.operation!=BLE_RELAY_OFF&&ble_relay_enabled()){
        flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("relay_active",EBUSY);return;
    }else if(request.operation!=BLE_RELAY_ON&&request.operation!=BLE_RELAY_OFF&&!carplay_ready()){
        flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("carplay_not_ready",EHOSTDOWN);return;
    }else if(request.operation==BLE_TEST&&request.test>BLE_TEST_LANES&&
             (!stable||!ble_expanded_icons(&snapshot.selected))){
        flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("test_icons_disabled",EPERM);return;
    }else {
        if(request.operation==BLE_RELAY_ON){
            request.peer=snapshot.selected;if(!request.peer.type)ble_saved_peer(&request.peer);
            if(request.peer.type<1||request.peer.type>2){flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("target_not_selected",ENOENT);return;}
        }
        if((request.operation==BLE_RELAY_ON||request.operation==BLE_RELAY_OFF)&&ble_relay_save(request.operation==BLE_RELAY_ON)){
            flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("settings_write_failed",errno);return;
        }
        if(request.operation==BLE_CONNECT){
            int selected=0;
            for(unsigned i=0;i<snapshot.peer_count&&i<BLE_PEERS;i++)if(snapshot.peers[i].type==request.peer.type&&!memcmp(snapshot.peers[i].address,request.peer.address,6)){
                request.peer=snapshot.peers[i];selected=request.peer.connectable;break;}
            if(!selected&&snapshot.selected.type==request.peer.type&&!memcmp(snapshot.selected.address,request.peer.address,6)){
                request.peer=snapshot.selected;selected=1;}
            if(!selected){flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("target_not_selected",ENOENT);return;}
        }
        __atomic_store_n(&s->cancel,request.operation==BLE_RELAY_OFF,__ATOMIC_RELEASE);s->request=request;
        __atomic_add_fetch(&s->request_seq,1,__ATOMIC_RELEASE);
        if(!running){
            pid_t child=fork();
            if(child<0){s->handled_seq=s->request_seq;flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);json_error("worker_start_failed",errno);return;}
            if(!child){
                setsid();int nullfd=open("/dev/null",O_RDWR);
                if(nullfd>=0){dup2(nullfd,0);dup2(nullfd,1);dup2(nullfd,2);if(nullfd>2)close(nullfd);}
                close(fd);char *args[]={BLE_PROGRAM,"--worker",NULL};execv(BLE_PROGRAM,args);_exit(127);
            }
            uint64_t ticks=ble_process_start((unsigned)child);s->start_low=(unsigned)ticks;s->start_high=(unsigned)(ticks>>32);
            __atomic_store_n(&s->worker_pid,(unsigned)child,__ATOMIC_RELEASE);s->worker_heartbeat=(unsigned)(ble_now()/1000);
        }
    }
    unsigned sequence=s->request_seq;flock(fd,LOCK_UN);munmap(s,BLE_BYTES);close(fd);
    json_headers();printf("{\"ok\":true,\"queued\":true,\"request\":%u}\n",sequence);
}
#ifndef U5BLE_NO_MAIN
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--worker"))return worker();
    const char *query=getenv("QUERY_STRING"),*method=getenv("REQUEST_METHOD");
    if(!query||!strcmp(query,"action=status")){
        if(method&&strcmp(method,"GET")&&strcmp(method,"HEAD")){json_error("get_required",EINVAL);return 0;}status();return 0;
    }
    if(strncmp(query,"action=",7)){json_error("unknown_action",EINVAL);return 0;}
    post(query+7);return 0;
}
#endif
