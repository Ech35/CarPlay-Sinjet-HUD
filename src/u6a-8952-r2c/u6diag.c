#include "u6diag.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include "ble_shared.h"
#include "nav_storage.h"
#include "carplay_gate.h"

static const char *state_stage="none";
static int state_errno;

static struct Diag *state_error(int fd,const char *stage,int code) {
    if (fd>=0) close(fd);
    state_stage=stage;
    state_errno=code;
    return NULL;
}

static int memory_backed(unsigned type) {
    return type==0x01021994u || type==0x858458f6u;
}

static struct Diag *open_state(void) {
    struct statfs fs;
    if (mkdir(DIAG_DIR,0700) && errno!=EEXIST) return state_error(-1,"ram_directory",errno);
    if (statfs(DIAG_DIR,&fs)) return state_error(-1,"ram_filesystem",errno);
    if (!memory_backed((unsigned)fs.f_type)) {
        unsigned long flags=MS_NODEV|MS_NOSUID|MS_NOEXEC;
        if (mount("u6diag",DIAG_DIR,"tmpfs",flags,"size=2097152,mode=0700") &&
            mount("u6diag",DIAG_DIR,"ramfs",flags,"mode=0700")) {
            /* A concurrent request can have mounted the directory already. */
            int failure=errno;
            if (statfs(DIAG_DIR,&fs) || !memory_backed((unsigned)fs.f_type))
                return state_error(-1,"ram_mount",failure);
        }
        if (statfs(DIAG_DIR,&fs)) return state_error(-1,"ram_filesystem",errno);
        if (!memory_backed((unsigned)fs.f_type)) return state_error(-1,"ram_backing_unconfirmed",EINVAL);
    }
    int fd=open(DIAG_PATH,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) return state_error(-1,"state_open",errno);
    if (flock(fd,LOCK_EX)) return state_error(fd,"state_lock",errno);
    /* The vendor ARM kernel may return EINVAL to statx. Use its native
     * llseek path to determine the length of our private RAM file. */
    off_t size=lseek(fd,0,SEEK_END);
    if (size<0) return state_error(fd,"state_seek",errno);
    if (size>(off_t)sizeof(struct Diag)) return state_error(fd,"state_size",EINVAL);
    if (lseek(fd,0,SEEK_SET)<0) return state_error(fd,"state_seek",errno);
    uint32_t marker=0;
    if (size>=(off_t)sizeof(marker)) {
        ssize_t bytes=read(fd,&marker,sizeof(marker));
        if (bytes!=(ssize_t)sizeof(marker)) return state_error(fd,"state_header",bytes<0?errno:EIO);
    }
    if (marker && (marker!=DIAG_MAGIC || size!=(off_t)sizeof(struct Diag)))
        return state_error(fd,"state_header",EINVAL);
    if (!marker) {
        /* Allocate actual RAM pages before mmap so a full tmpfs reports ENOSPC
         * as an initialization error instead of SIGBUS during initialization. */
        static const unsigned char zeros[4096];
        if (lseek(fd,0,SEEK_SET)<0) return state_error(fd,"state_seek",errno);
        for (size_t used=0;used<sizeof(struct Diag);) {
            size_t count=sizeof(struct Diag)-used;
            if (count>sizeof(zeros)) count=sizeof(zeros);
            ssize_t bytes=write(fd,zeros,count);
            if (bytes<0 && errno==EINTR) continue;
            if (bytes!=(ssize_t)count) return state_error(fd,"state_allocate",bytes<0?errno:ENOSPC);
            used+=count;
        }
    }
    struct Diag *d=mmap(NULL,sizeof(*d),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (d==MAP_FAILED) return state_error(fd,"state_map",errno);
    if (!marker) {
        d->enabled=1;
        __atomic_store_n(&d->magic,DIAG_MAGIC,__ATOMIC_RELEASE);
    }
    close(fd);
    return d;
}

static uint32_t current(const uint32_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static void headers(const char *type) {
    printf("Content-Type: %s\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n",type);
}
static void status(struct Diag *d) {
    headers("application/json; charset=utf-8");
    int carplay=access(CARPLAY_MARKER,F_OK)==0;
    if (!d) {
        printf("\r\n{\"ok\":false,\"carplay\":%s,\"error\":\"%s\",\"errno\":%d}\n",
               carplay?"true":"false",state_stage,state_errno); return;
    }
    unsigned head=current(&d->head), cleared=current(&d->cleared), session=current(&d->session);
    unsigned count=head>=cleared?head-cleared:0;
    printf("\r\n{\"ok\":true,\"carplay\":%s,\"logging\":%s,\"phase\":%u,\"session\":%u,"
           "\"recorded\":%u,\"capacity\":%u,\"overwritten\":%u,\"identify\":%u,\"accepted\":%u,"
           "\"rejected\":%u,\"subscriptions\":%u,\"route\":%u,\"turn\":%u,\"lane\":%u,\"invalid\":%u,"
           "\"producer_pid\":%u,"
           "\"auth_complete\":%s,\"callback_ready\":%s,"
           "\"control_tx\":%u,\"carplay_calls\":%u,\"carplay_returns\":%u,\"carplay_return\":%d,\"carplay_skipped\":%u}\n",
           carplay?"true":"false",current(&d->enabled)?"true":"false",current(&d->phase),
           session,count>DIAG_CAPACITY?DIAG_CAPACITY:count,DIAG_CAPACITY,
           count>DIAG_CAPACITY?count-DIAG_CAPACITY:0,current(&d->ident_sent),current(&d->accepted),
           current(&d->rejected),current(&d->subscriptions),current(&d->nav_route),
           current(&d->nav_turn),current(&d->nav_lane),current(&d->nav_invalid),
           current(&d->producer_pid),
           session && current(&d->auth_complete_session)==session?"true":"false",
           current(&d->callback_ready)?"true":"false",
           current(&d->control_tx),current(&d->cp_calls),current(&d->cp_returns),(int32_t)current(&d->cp_return),
           current(&d->cp_skipped));
}

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0]<<8)|p[1]; }
static unsigned be32(const unsigned char *p) { return (be16(p)<<16)|be16(p+2); }

static void describe_nav(const struct EventRecord *r) {
    if (r->raw_bytes<6 || r->type!=NAV_RX) return;
    unsigned count=0;
    for (unsigned offset=6;offset+4<=r->raw_bytes;) {
        unsigned length=be16(r->raw+offset), tag=be16(r->raw+offset+2);
        if (length<4 || length>r->raw_bytes-offset) break;
        const unsigned char *v=r->raw+offset+4;
        unsigned n=length-4;
        if (!tag && n==2) printf(" component=0x%04x",be16(v));
        if (!tag && r->message==0x5201 && n>2 && !(n&1)) {
            fputs(" components=",stdout);
            for (unsigned i=0;i<n;i+=2) printf("%s0x%04x",i?",":"",be16(v+i));
        }
        if (r->message==0x5201) {
            if (tag==1 && n==1) printf(" route_state=%u",v[0]);
            if (tag==2 && n==1) printf(" maneuver_state=%u",v[0]);
            if (tag==10 && n==4) printf(" distance_to_maneuver=%u",be32(v));
        } else if (r->message==0x5202) {
            if (tag==1 && n==2) printf(" maneuver_index=%u",be16(v));
            if (tag==3 && n==1) printf(" maneuver_type=%u",v[0]);
            if (tag==11 && n==2) printf(" exit_angle=%d",(int)(int16_t)be16(v));
        } else if (r->message==0x5204) {
            if (tag==1 && n==2) printf(" lane_group=%u",be16(v));
            if (tag==2) ++count;
        }
        offset+=length;
    }
    if (r->message==0x5204) printf(" lanes_in_sample=%u",count);
}

static const char *event_name(unsigned i) {
    static const char *const names[]={"identify_start","identify_sent","identify_accepted",
        "identify_rejected","wifi_write_callback_present","subscribe_attempt","subscribe_result","navigation_rx",
        "navigation_invalid","session_ended","other_iap_transport","native_auth_success","carplay_marker_observed",
        "identify_component_handoff","identify_tx_handoff","identify_rx_handoff","subscribe_control_handoff",
        "carplay_send_enter","carplay_send_return","carplay_send_skipped","nav_frame_pending","nav_frame_oversized",
        "native_auth_complete"};
    return i<sizeof(names)/sizeof(*names)?names[i]:"unknown";
}
static const char *identify_reason(unsigned i) {
    static const char *const names[]={"component_added","stock_component_present","missing_message_list",
        "malformed_identity","no_buffer_space","component_id_collision","stock_retry_after_rejection"};
    return i<sizeof(names)/sizeof(*names)?names[i]:"unknown";
}

static void export(struct Diag *d,int download) {
    headers("text/plain; charset=utf-8");
    if (download) fputs("Content-Disposition: attachment; filename=\"" DIAG_LOG_FILENAME "\"\r\n",stdout);
    printf("\r\nbuild=%s\nproject=ly8952\nbase_version=26052617.8952.1\n",DIAG_BUILD);
    if (!d) {
        printf("diagnostic_state=unavailable\nstate_stage=%s\nstate_errno=%d\n",state_stage,state_errno);
        return;
    }
    unsigned head=current(&d->head), first=current(&d->cleared)+1, session=current(&d->session);
    if (head>=DIAG_CAPACITY && first<=head-DIAG_CAPACITY) first=head-DIAG_CAPACITY+1;
    if (!download && head>=48 && first<=head-48) first=head-47;
    printf("logging=%u\ncarplay_marker=%d\nsession=%u\nphase=%u\nobserved_iap_modes=0x%x\n"
           "wifi_write_callback_ready=%u\nnative_pass_mode=%u\nidentify_added=%u\nidentify_accepted=%u\n"
           "identify_rejected=%u\nsubscription_attempts=%u\nrx_5201=%u\nrx_5202=%u\nrx_5204=%u\n"
           "nav_invalid=%u\nproducer_pid=%u\nsubscription_trigger=after_native_auth_callback_return\ncontrol_channel=%u\n"
           "native_auth_success=%u\nnative_auth_complete=%u\ncarplay_marker_seen=%u\nlast_submit_return=%d\ncontrol_tx_5200=%u\n"
           "carplay_send_calls=%u\ncarplay_send_returns=%u\nlast_carplay_send_return=%d\ncarplay_send_skipped=%u\n"
           "rx_control_chunks=%u\nrx_other_chunks=%u\nnav_fragmented=%u\nnav_combined=%u\nnav_oversized=%u\n"
           "route_for_7f01=%u\nrecord_range=%u..%u\nrecord_capacity=%u\n\n[timeline]\n",
           current(&d->enabled),access(CARPLAY_MARKER,F_OK)==0,session,current(&d->phase),
           current(&d->types_seen),current(&d->callback_ready),current(&d->pass_mode),
           current(&d->ident_sent),current(&d->accepted),current(&d->rejected),current(&d->subscriptions),
           current(&d->nav_route),current(&d->nav_turn),current(&d->nav_lane),current(&d->nav_invalid),
           current(&d->producer_pid),current(&d->control_channel),
           current(&d->auth_success),(unsigned)(session && current(&d->auth_complete_session)==session),
           current(&d->marker_seen),(int32_t)current(&d->submit_return),current(&d->control_tx),
           current(&d->cp_calls),current(&d->cp_returns),(int32_t)current(&d->cp_return),current(&d->cp_skipped),
           current(&d->rx_control_chunks),current(&d->rx_other_chunks),current(&d->rx_fragmented),current(&d->rx_combined),
           current(&d->rx_oversized),current(&d->route_7f01),
           first,head,DIAG_CAPACITY);
    for (unsigned seq=first;seq && seq<=head;++seq) {
        const struct EventRecord *slot=&d->records[(seq-1)&(DIAG_CAPACITY-1)];
        if (current(&slot->stamp)!=seq) continue;
        struct EventRecord row;
        memcpy(&row,slot,sizeof(row));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (current(&slot->stamp)!=seq || row.raw_bytes>DIAG_RAW_BYTES) continue;
        printf("[%u.%03u] seq=%u session=%u event=%s message=0x%04x detail=%d extra=%u",
               row.time_ms/1000,row.time_ms%1000,seq,row.session,event_name(row.type),row.message,
               (int32_t)row.detail,row.extra);
        if (row.type==IDENT_SENT) printf(" result=%s identity_bytes=%u",identify_reason(row.detail),row.extra);
        if (row.type==AUTH_SUCCESS) printf(" native_state=%u callback_installed=%u",row.detail,row.extra);
        if (row.type==AUTH_COMPLETE) printf(" native_state=%u",row.detail);
        if (row.type==MARKER_OBSERVED) printf(" present=%u initial_sample=%u",row.detail,row.extra);
        if (row.type==SUBMIT_TRY) fputs(" trigger=auth_callback_return",stdout);
        if (row.type==NAV_RX || row.type==NAV_INVALID)
            printf(" channel=%u wire_bytes=%u captured=%u",row.extra,row.frame_bytes,row.raw_bytes);
        if (row.type==NAV_INCOMPLETE || row.type==NAV_OVERSIZED)
            printf(" channel=%u expected_bytes=%u observed_bytes=%u",row.extra,row.detail,row.frame_bytes);
        if (row.type==CP_SEND_ENTER || row.type==CP_SEND_RETURN || row.type==CP_SEND_SKIPPED)
            printf(" link_sequence=%u channel=%u continued_match=%u packet_bytes=%u",
                   row.extra&255,(row.extra>>8)&255,row.extra>>16,row.frame_bytes);
        describe_nav(&row);
        putchar('\n');
        if (row.raw_bytes) {
            fputs("  raw_hex=",stdout);
            for (unsigned i=0;i<row.raw_bytes;++i) printf("%02x",row.raw[i]);
            putchar('\n');
        }
    }
    puts("[end_timeline]");
    u6_carplay_gate_dump(d);
    ble_dump();
}

int main(int argc,char **argv) {
    struct Diag *d=open_state();
    if (argc==2 && !strcmp(argv[1],"--init")) {
        if(!d)return 1;
        struct NavRing *ring=nav_open();
        if(!ring)return 1;
        munmap(ring,NAV_BYTES);
        return 0;
    }
    const char *q=getenv("QUERY_STRING"), *method=getenv("REQUEST_METHOD");
    if (!q || !*q || !strcmp(q,"action=status")) {
        status(d);
    } else if (!strcmp(q,"action=download") || !strcmp(q,"action=preview")) {
        export(d,!strcmp(q,"action=download"));
    } else if (method && !strcmp(method,"POST") &&
               (!strcmp(q,"action=init") || !strcmp(q,"action=clear") ||
                !strcmp(q,"action=enable") || !strcmp(q,"action=disable"))) {
        if (d && !strcmp(q,"action=clear")) {
            __atomic_store_n(&d->cleared,current(&d->head),__ATOMIC_RELEASE);
            ble_mark_clear();
        }
        if (d && (!strcmp(q,"action=enable") || !strcmp(q,"action=disable")))
            __atomic_store_n(&d->enabled,!strcmp(q,"action=enable"),__ATOMIC_RELEASE);
        if (d && !strcmp(q,"action=init")) {
            struct NavRing *ring=nav_open();
            if(ring)munmap(ring,NAV_BYTES);
        }
        status(d);
    } else {
        fputs("Status: 400 Bad Request\r\n",stdout);
        headers("application/json; charset=utf-8");
        fputs("\r\n{\"ok\":false}\n",stdout);
    }
    return 0;
}
