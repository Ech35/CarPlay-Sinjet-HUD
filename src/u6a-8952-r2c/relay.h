/* One worker owns the core and the ATT socket. The iAP hook only enqueues
 * bounded navigation frames in RAM, independently of diagnostic logging. */
#include "nav_storage.h"
static hud_core *nav_core;
static struct NavRing *nav_ring;
static struct NavState live_nav;
static unsigned nav_cursor,nav_epoch,nav_dropped,nav_oversize,nav_initialized;
static int relay_flushing,relay_reading,relay_close_pending,relay_clear_pending;
static int64_t relay_retry_at,relay_input_log_at,relay_tx_log_at;
static int64_t relay_normal_retry_at;
static int relay_recovering;
static unsigned relay_retry_count;
static unsigned char relay_tx_frame[130];
static unsigned relay_tx_used;
static int64_t relay_last_input_ms,relay_last_turn_ms,relay_refresh_request_ms;
static int64_t relay_icon_check_at;
static unsigned relay_logged_sequence;
#define RELAY_REFRESH_MS 1000
#define RELAY_FRESHNESS_MS 120000
#define RELAY_RESUME_DISTANCE_MS 5000
#define RELAY_FIRST_RETRY_MS 5000
#define RELAY_FAST_WAIT_MS 1500
#define RELAY_FAST_BUDGET_MS 3000

static void relay_sync_link(void){
    if(!nav_core||!view.relay_enabled||att<0||!view.connected)return;
    if(hc_phase(nav_core)==HC_SCANNING){
        unsigned g=hc_generation(nav_core);uint64_t now=(uint64_t)ble_now();
        int expanded=ble_expanded_icons(&view.selected);
        u5_core_set_expanded_icons(nav_core,expanded);
        relay_icon_check_at=(int64_t)now+RELAY_REFRESH_MS;
        hc_found(nav_core,g,now);hc_connected(nav_core,g,now);hc_ready(nav_core,g,now);u5_core_refresh(nav_core);
        event("relay_ready",0,"navigation converter attached; expanded_icons=%d",expanded);
    }
    view.relay_core_phase=(unsigned)hc_phase(nav_core);
}
static int same_source(const char *a,const char *b){
    return !strcmp(a,b)||(!strncmp(a,"Apple",5)&&!strncmp(b,"Apple",5));
}
static int relay_decode(const unsigned char *p,unsigned n,unsigned sequence,uint64_t received_ms){
    unsigned mid=le16(p+4);mid=(mid>>8)|(mid&255)<<8;
    if(mid==0x5201){
        rgd_update_t u;if(!rgd_parse_update(p,n,&u)||((u.present&RGD_UPD_ROUTE_STATE)&&u.route_state>6))return 0;
        if(u.present&RGD_UPD_SOURCE_NAME){
            if(view.relay_source[0]&&!same_source(view.relay_source,u.source_name)){
                u5_core_session_end(nav_core);if(view.relay_enabled)event("relay_source_changed",0,"old maneuver and lane caches cleared");
            }
            copy_text(view.relay_source,sizeof(view.relay_source),u.source_name);
        }
        if(u.present&RGD_UPD_ROUTE_STATE)view.relay_route_state=u.route_state;
        if(u.present&RGD_UPD_DIST_TO_MANEUVER)view.relay_distance=u.dist_to_maneuver;
        if(view.relay_enabled&&(ble_now()-relay_input_log_at>=5000||((u.present&RGD_UPD_ROUTE_STATE)&&(u.route_state==0||u.route_state==2)))){
            relay_input_log_at=ble_now();event("relay_input",0,"seq=%u mid=5201 state=%u distance_m=%u",sequence,view.relay_route_state,view.relay_distance);
        }
    }else if(mid==0x5202){
        rgd_maneuver_t m;if(!rgd_parse_maneuver(p,n,&m))return 0;
        if(view.relay_enabled)event("relay_maneuver",0,"seq=%u index=%u type=%u present=%llx",sequence,m.index,m.maneuver_type,(unsigned long long)m.present);
    }else if(mid==0x5204){
        rgd_lane_guidance_t l;if(!rgd_parse_lane_guidance(p,n,&l))return 0;
        if(view.relay_enabled)event("relay_lanes",0,"seq=%u group=%u count=%u complete=%u",sequence,l.lane_guidance_index,(unsigned)l.lane_count,l.lane_complete);
    }else return 0;
    u5_core_input(nav_core,p,n,(uint64_t)ble_now(),received_ms);
    relay_last_input_ms=ble_now();view.relay_input_last_seconds=(unsigned)(relay_last_input_ms/1000);
    view.relay_freshness_expired=0;
    struct U5Selection choice;u5_core_selection(nav_core,&choice);
    if(choice.raw_index!=view.relay_raw_index||choice.selected_index!=view.relay_selected_index||
       choice.raw_type!=view.relay_raw_type||choice.selected_type!=view.relay_selected_type){
        view.relay_raw_index=choice.raw_index;view.relay_selected_index=choice.selected_index;
        view.relay_raw_type=choice.raw_type;view.relay_selected_type=choice.selected_type;
        if(view.relay_enabled)event("relay_selection",0,"raw=%u type=%u selected=%u type=%u reason=0",choice.raw_index,choice.raw_type,choice.selected_index,choice.selected_type);
    }
    return 1;
}
static void relay_ingest(void){
    if(!nav_core||!nav_ring||relay_in_write||relay_flushing||relay_reading)return;
    relay_reading=1;struct NavState state;
    if(!nav_state(nav_ring,&state))goto done;
    unsigned was_active=live_nav.active;live_nav=state;
    if(!nav_initialized){
        nav_initialized=1;nav_epoch=state.epoch;nav_dropped=state.dropped;nav_oversize=state.oversize;
        nav_cursor=state.write_seq>NAV_SLOTS?state.write_seq-NAV_SLOTS:0;
        hc_transport_reset(nav_core,view.relay_enabled&&state.active);
    }else if(state.epoch!=nav_epoch){
        nav_epoch=state.epoch;u5_core_session_end(nav_core);view.relay_source[0]=0;view.relay_route_state=0;
        if(att>=0&&view.relay_enabled)relay_close_pending=1;
        else hc_transport_reset(nav_core,view.relay_enabled&&state.active);
        event("relay_session",0,"epoch=%u active=%u pid=%u",state.epoch,state.active,state.pid);
    }else if(state.active!=was_active){
        /* Accepting the identity precedes the completed subscription.
         * Activation uses the same epoch after native authentication. */
        if(!state.active){u5_core_session_end(nav_core);if(att>=0&&view.relay_enabled)relay_close_pending=1;}
        hc_transport_reset(nav_core,view.relay_enabled&&state.active);
        if(state.active)relay_retry_at=0;
        event("relay_session",0,"epoch=%u active=%u pid=%u",state.epoch,state.active,state.pid);
    }
    view.relay_epoch=nav_epoch;
    if(state.dropped!=nav_dropped||state.oversize!=nav_oversize||(unsigned)(state.write_seq-nav_cursor)>NAV_SLOTS){
        nav_dropped=state.dropped;nav_oversize=state.oversize;nav_cursor=state.write_seq;
        view.relay_gaps++;u5_core_session_end(nav_core);
        if(att>=0&&view.relay_enabled)relay_clear_pending=1;
        else hc_transport_reset(nav_core,view.relay_enabled&&state.active);
        event("relay_input_gap",0,"epoch=%u through_seq=%u; stale caches cleared",nav_epoch,nav_cursor);goto done;
    }
    if(relay_close_pending||relay_clear_pending)goto done;
    if(!state.active){nav_cursor=state.write_seq;goto done;}
    while(nav_cursor!=state.write_seq){
        unsigned seq=nav_cursor+1;const struct NavSlot *slot=&nav_ring->slots[(seq-1)%NAV_SLOTS];
        unsigned stamp=__atomic_load_n(&slot->stamp,__ATOMIC_ACQUIRE);
        if(stamp!=seq*2)break;
        unsigned epoch=slot->epoch,n=slot->size,seconds=slot->seconds,milliseconds=slot->milliseconds;
        unsigned char data[NAV_MAX];
        if(n<6||n>NAV_MAX){view.relay_bad++;nav_cursor=seq;continue;}
        memcpy(data,slot->data,n);__atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(stamp!=__atomic_load_n(&slot->stamp,__ATOMIC_RELAXED))break;
        nav_cursor=seq;if(epoch!=nav_epoch)continue;
        int64_t received_ms=(int64_t)seconds*1000+milliseconds;
        int64_t age=ble_now()-received_ms;
        if(age<0||age>15000){view.relay_stale++;continue;}
        view.relay_sequence=seq;view.relay_input_count++;
        if(!relay_decode(data,n,seq,(uint64_t)received_ms)){view.relay_bad++;event("relay_parse_failed",EPROTO,"seq=%u size=%u",seq,n);}
    }
done:relay_reading=0;
}
static void relay_record_tx(const unsigned char *p,unsigned n,int cleanup){
    if(n>sizeof(relay_tx_frame)-relay_tx_used){relay_tx_used=0;return;}
    memcpy(relay_tx_frame+relay_tx_used,p,n);relay_tx_used+=n;
    if(relay_tx_used<3||relay_tx_used<relay_tx_frame[2]+4u)return;
    unsigned size=relay_tx_frame[2]+4u;unsigned char *f=relay_tx_frame;
    if(size==relay_tx_used&&f[0]==255&&f[1]==85){
        view.relay_frames++;view.relay_last_tx=(unsigned)(ble_now()/1000);view.relay_last_command=f[4];
        if(f[4]==0&&size==11){
            relay_last_turn_ms=ble_now();
            view.relay_icon=f[5];view.relay_distance=le32(f+6);
            if(!cleanup)link_navigation_written(view.relay_icon,view.relay_distance,!!(view.write_properties&8));
            if (view.relay_sequence != relay_logged_sequence && relay_last_turn_ms - relay_tx_log_at >= 1000) {
                relay_tx_log_at = relay_last_turn_ms;
                relay_logged_sequence = view.relay_sequence;
                event("relay_turn_sent",0,"seq=%u icon=%u distance_m=%u",view.relay_sequence,f[5],le32(f+6));}
        }else if(f[4]==0xe0&&size==7){
            view.relay_clear_sent=f[5]==0;
            event(f[5]?"relay_display_start":"relay_display_clear",0,"seq=%u write_response=%u",view.relay_sequence,!!(view.write_properties&8));
        }
    }relay_tx_used=0;
}
static int relay_write_step(int cleanup){
    if(!nav_core||att<0||!view.connected)return 0;
    unsigned char chunk[20];unsigned n=(unsigned)hc_next_chunk(nav_core,(uint64_t)ble_now(),chunk,sizeof(chunk));
    if(!n)return 0;
    unsigned g=hc_generation(nav_core);relay_in_write=1;
    int rc=write_frame(chunk,n,cleanup);int failure=errno;relay_in_write=0;
    int64_t now=ble_now();
    unsigned action=hc_written(nav_core,g,!rc,(uint64_t)now);
    if(rc||action&HC_CLOSE){errno=failure?failure:EIO;event("relay_write_failed",errno,"seq=%u chunk=%u",view.relay_sequence,n);return -1;}
    link_watch.last_navigation_write_ms=now;
    view.relay_chunks++;relay_record_tx(chunk,n,cleanup);publish();return 1;
}
static void relay_stop_link(int clear_cache){
    if(nav_core&&att>=0&&view.connected){
        relay_flushing=1;
        if(clear_cache)u5_core_session_end(nav_core);else u5_core_clear_display(nav_core);
        view.relay_clear_sent=0;
        relay_write_deadline=ble_now()+1500;unsigned empty=0;
        while(ble_now()<relay_write_deadline){
            int result=relay_write_step(1);if(result<0)break;
            if(!result){if(++empty>=2)break;}else empty=0;
            struct timespec pause={.tv_nsec=16000000};nanosleep(&pause,NULL);
        }
        relay_write_deadline=0;relay_flushing=0;
    }else if(nav_core&&clear_cache)u5_core_session_end(nav_core);
    close_hud(!view.relay_clear_sent);relay_tx_used=0;
    if(nav_core)hc_transport_reset(nav_core,view.relay_enabled&&live_nav.active);
    relay_close_pending=relay_clear_pending=0;
    relay_recovering=0;relay_normal_retry_at=0;view.relay_retry_seconds=0;
}
static unsigned retry_remaining_ms(int64_t at,int64_t now){
    return at>now?(unsigned)(at-now):0;
}
static void relay_link_failed(unsigned error,const char *stage){
    int64_t now=ble_now();
    unsigned delay_ms;
    if(relay_normal_retry_at){
        /* The extra attempt uses only time before the original retry slot. */
        delay_ms=retry_remaining_ms(relay_normal_retry_at,now);
        relay_normal_retry_at=0;
    }else{
        delay_ms=relay_retry_count<3?RELAY_FIRST_RETRY_MS<<relay_retry_count:30000u;
        if(att>=0&&view.connected&&view.relay_enabled&&live_nav.active){
            relay_recovering=1;
            if(!relay_retry_count&&relay_last_input_ms&&now-relay_last_input_ms<RELAY_FRESHNESS_MS&&u5_core_has_guidance(nav_core)){
                relay_normal_retry_at=now+delay_ms;
                delay_ms=RELAY_FAST_WAIT_MS;
            }
        }
        if(relay_retry_count<1000)relay_retry_count++;
    }
    link_note(link_watch.attempt, BL_RETRY, (int)error, (int)delay_ms, relay_normal_retry_at!=0);
    close_hud(0);relay_tx_used=0;
    if(nav_core)hc_transport_reset(nav_core,view.relay_enabled&&live_nav.active);
    view.relay_retries++;view.relay_retry_seconds=(delay_ms+999)/1000;relay_retry_at=now+delay_ms;
    view.phase=BS_ERROR;event(stage,error,"retry after %ums",delay_ms);
}
static void relay_service(void){
    if(!nav_core)return;relay_ingest();
    if(!view.relay_enabled)return;
    int ready=carplay_ready()&&live_nav.active&&live_nav.pid==cached_session_pid;
    if(relay_close_pending||relay_clear_pending){relay_stop_link(1);view.phase=BS_IDLE;relay_retry_at=ble_now()+1000;return;}
    if(!ready){
        if(att>=0){relay_stop_link(1);view.phase=BS_IDLE;event("relay_wait_carplay",0,"HUD stopped; waiting for active CarPlay session");}
        relay_recovering=0;relay_normal_retry_at=0;
        return;
    }
    int64_t now=ble_now();
    if(att<0&&now>=relay_retry_at){
        if(view.selected.type<1||view.selected.type>2){event("relay_select_hud",0,"connect the intended HUD once before enabling relay");relay_retry_at=ble_now()+30000;return;}
        if(relay_normal_retry_at){
            connection_deadline=relay_retry_at+RELAY_FAST_BUDGET_MS;
            if(now>=connection_deadline||!u5_core_has_guidance(nav_core)){
                connection_deadline=0;
                relay_retry_at=relay_normal_retry_at;relay_normal_retry_at=0;
                view.relay_retry_seconds=(retry_remaining_ms(relay_retry_at,now)+999)/1000;
                return;
            }
        }
        view.busy=1;publish();int rc=connect_hud(&view.selected);int failure=errno;view.busy=0;
        connection_deadline=0;
        if(rc){relay_link_failed(failure,"relay_connect_failed");return;}
        relay_retry_count=0;view.relay_retry_seconds=0;relay_retry_at=relay_normal_retry_at=0;
        relay_ingest();
        if(relay_close_pending||relay_clear_pending||!carplay_ready()||!live_nav.active||live_nav.pid!=cached_session_pid){relay_stop_link(1);return;}
        if(relay_recovering){
            relay_recovering=0;
            if(u5_core_resume(nav_core,(uint64_t)ble_now(),RELAY_RESUME_DISTANCE_MS))
                event("relay_resume_cached",0,"restoring latest current maneuver");
            else event("relay_resume_wait",0,"waiting for current maneuver and fresh distance");
        }
    }
    if(att>=0){
        relay_sync_link();
        unsigned action=hc_tick(nav_core,(uint64_t)ble_now());
        if(action&HC_CLOSE){relay_link_failed(ETIMEDOUT,"relay_core_timeout");return;}
        now=ble_now();
        if(relay_last_input_ms&&now-relay_last_input_ms>=RELAY_FRESHNESS_MS&&!view.relay_freshness_expired){
            u5_core_clear_display(nav_core);view.relay_freshness_expired=1;
            event("relay_freshness_expired",0,"no valid navigation input for 120s; display refresh suspended");
        }
        if(hc_phase(nav_core)==HC_READY&&now>=relay_icon_check_at){
            /* Read the saved setting at most once per refresh interval.
             * Rendering uses the existing queue, including any in-flight chunk. */
            relay_icon_check_at=now+RELAY_REFRESH_MS;
            int expanded=ble_expanded_icons(&view.selected);
            if(u5_core_set_expanded_icons(nav_core,expanded)){
                if(!view.relay_freshness_expired&&u5_core_refresh(nav_core))relay_refresh_request_ms=now;
                event("relay_icons_changed",0,"expanded_icons=%d",expanded);
            }
        }
        int64_t recent=relay_last_turn_ms>relay_refresh_request_ms?relay_last_turn_ms:relay_refresh_request_ms;
        if(!view.relay_freshness_expired&&now-recent>=RELAY_REFRESH_MS&&u5_core_refresh(nav_core)){
            relay_refresh_request_ms=now;view.relay_keepalives++;view.relay_keepalive_last_seconds=(unsigned)(now/1000);
        }
        if(relay_write_step(0)<0){unsigned e=errno;relay_link_failed(e,"relay_send_retry");}
    }
    view.relay_core_phase=(unsigned)hc_phase(nav_core);
}
