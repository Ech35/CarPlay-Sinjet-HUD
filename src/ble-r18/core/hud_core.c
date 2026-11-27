/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native deterministic core; no file I/O, threads, radio access or log output.
 * Platform integration is intentionally separate and still required.
 */
#include "hud_core.h"
#include "vendor/rgd/rgd_tlv.h"
#include "../../hud-icons-r3/icon_ids.h"
#include <stdlib.h>

#define SLOTS 32
#define LSLOTS 8
#define QMAX 16
#define FMAX 130
typedef struct { uint8_t bytes[FMAX]; uint16_t n; } frame;
typedef struct { int used; uint64_t age; rgd_maneuver_t v; } man_slot;
typedef struct { int used; uint64_t age; rgd_lane_guidance_t v; } lane_slot;
struct hud_core {
    uint16_t component;
    int phase,attempts,active,demand,display_on,write_busy,available;
    int expanded_icons;
    uint32_t generation;
    uint64_t deadline,next_write,write_deadline,age,distance_received_ms;
    rgd_update_t route;
    man_slot mans[SLOTS];
    lane_slot lanes[LSLOTS];
    frame pending[QMAX],current;
    size_t queued,offset,in_flight;
};

static unsigned scan(hud_core *c,uint64_t now) {
    c->phase=HC_SCANNING;c->attempts++;c->generation++;c->deadline=now+15000;
    return HC_SCAN;
}
static unsigned demand(hud_core *c,uint64_t now) {
    c->demand=1;
    if(!c->available||c->phase!=HC_IDLE)return 0;
    c->attempts=0;return scan(c,now);
}
static void drop_queue(hud_core *c) {
    c->queued=0;c->current.n=0;c->offset=0;c->in_flight=0;c->next_write=0;c->display_on=0;c->write_busy=0;
}
static int pack(frame *f,const uint8_t *p,size_t n) {
    if(!n||n+5>FMAX)return 0;
    f->n=(uint16_t)(n+5);f->bytes[0]=255;f->bytes[1]=85;
    f->bytes[2]=(uint8_t)(n+1);f->bytes[3]=1;memcpy(f->bytes+4,p,n);
    uint8_t sum=0;for(size_t i=2;i<n+4;i++)sum=(uint8_t)(sum+f->bytes[i]);
    f->bytes[n+4]=sum;return 1;
}
static void enqueue(hud_core *c,const uint8_t *p,size_t n) {
    frame f;if(!pack(&f,p,n))return;
    /* Every command produced by this core is replaceable navigation state. */
    size_t dest=0;int stop=p[0]==0xe0&&n>1&&!p[1];
    /* Selected but not accepted by the stack (BUSY at offset zero) is still
     * unsent. Replace it too, without extending the existing busy deadline.
     * Never overwrite any frame that has accepted/in-flight bytes. */
    int replace=c->current.n&&!c->offset&&!c->in_flight&&c->current.bytes[4]==p[0];
    if(stop&&c->current.n&&!c->offset&&!c->in_flight)c->current.n=0;
    for(size_t i=0;i<c->queued;i++)
        if(!stop&&c->pending[i].bytes[4]!=p[0])c->pending[dest++]=c->pending[i];
    c->queued=dest;
    if(replace&&!stop){c->current=f;return;}
    if(c->queued<QMAX)c->pending[c->queued++]=f;
}
static void clear_lanes(hud_core *c) {
    const uint8_t simple[]={0x11,0},v1[]={0x12},v2[]={0x15};
    enqueue(c,simple,2);enqueue(c,v1,1);enqueue(c,v2,1);
}
static void clear_nav(hud_core *c) {
    const uint8_t end[]={0xe0,0},speed[]={1,0,0,0,0},cam[]={0x13,0,0,0,0},old[]={0x10,0,0,0,0};
    c->display_on=0;enqueue(c,end,2);clear_lanes(c);enqueue(c,speed,5);enqueue(c,cam,5);enqueue(c,old,5);
}
static int accepts(hud_core *c,const uint16_t *ids,size_t n) {
    if(!n)return 1;
    for(size_t i=0;i<n;i++)if(ids[i]==c->component)return 1;
    return 0;
}
static int current_index(hud_core *c) {
    return c->route.maneuver_list_count?c->route.maneuver_list[0]:-1;
}
static int guidance_route(const hud_core *c) {
    return c->route.route_state==RGD_STATE_ROUTE_SET||c->route.route_state==RGD_STATE_PROCEED_TO_ROUTE;
}
static man_slot *maneuver(hud_core *c,int index,int create) {
    man_slot *victim=NULL;uint64_t oldest=UINT64_MAX;
    for(size_t i=0;i<SLOTS;i++) {
        man_slot *s=&c->mans[i];
        if(s->used&&s->v.index==index)return s;
        if(!s->used){victim=s;oldest=0;}
        else if(oldest&&s->v.index!=current_index(c)&&s->age<oldest){victim=s;oldest=s->age;}
    }
    if(!create||!victim)return NULL;
    memset(victim,0,sizeof(*victim));victim->used=1;victim->v.index=(uint16_t)index;return victim;
}
static lane_slot *lane(hud_core *c,int index,int create) {
    lane_slot *victim=NULL;uint64_t oldest=UINT64_MAX;
    for(size_t i=0;i<LSLOTS;i++) {
        lane_slot *s=&c->lanes[i];
        if(s->used&&s->v.lane_guidance_index==index)return s;
        if(!s->used){victim=s;oldest=0;}
        else if(oldest&&s->v.lane_guidance_index!=c->route.lane_guidance_index&&s->age<oldest){victim=s;oldest=s->age;}
    }
    if(!create||!victim)return NULL;
    memset(victim,0,sizeof(*victim));victim->used=1;victim->v.lane_guidance_index=(uint16_t)index;return victim;
}

static int turn_from_angle(int angle) {
    /* Seven integer-degree bins approved in the review workbook. Ordinary
     * +/-180 degrees remains a sharp turn; only explicit U-turn/roundabout
     * families below are allowed to select a U-turn image from the angle. */
    if(angle<=-113)return 6;
    if(angle<=-68)return 2;
    if(angle<=-23)return 4;
    if(angle<=22)return 9;
    if(angle<=67)return 5;
    if(angle<=112)return 3;
    return 7;
}
API int hc_turn_icon_angle(unsigned t,int angle,int present) {
    /* Approved 2026-10-31: Sinjet BLE image IDs, NOT CarPlay/AMap/BAP IDs.
     * Mainland right-hand traffic is fixed. Ordinary BLE can only select
     * images 2..19; no extended CAN-only image is emitted by this table. */
    int valid=present&&angle>=-180&&angle<=180;
    if(t==7||(t>=28&&t<=46)) {
        if(!valid)return 12;
        /* Source angles are integral; >=157.5 degrees means >=158 here. */
        if(angle<=-158||angle>=158)return 8;
        return turn_from_angle(angle);
    }
    switch(t) {
    case 0:case 3:case 5:case 15:case 17:return 9;
    case 1:return 2;
    case 2:return 3;
    /* Icon 7 is only an APPROXIMATION of a right U-turn (no exact BLE image).
     * 0 or an absent/invalid angle falls back to the mainland left U-turn. */
    case 4:case 18:case 26:return valid&&angle>0?7:8;
    case 6:return 11;
    case 19:return 8; /* roundabout return, never a pedestrian/stairs image */
    case 8:case 9:return valid&&angle!=0?(angle<0?4:5):9;
    case 11:case 16:case 51:return valid?turn_from_angle(angle):9;
    case 20:return valid&&angle<0?(angle>=-67?4:angle>=-112?2:6):2;
    case 21:return valid&&angle>0?(angle<=67?5:angle<=112?3:7):3;
    case 10:case 12:case 24:case 25:case 27:return 15;
    case 13:case 22:case 49:case 52:return 4;
    case 14:case 23:case 50:case 53:return 5;
    case 47:return 6;
    case 48:return 7;
    /* Explicit user override after workbook review: 54..254 and 255 use the
     * tunnel picture as a neutral placeholder, NOT a route-end signal. */
    default:return 16;
    }
}
API int hc_turn_icon(unsigned t,int side) {
    (void)side;return hc_turn_icon_angle(t,0,0);
}

static int expanded_turn_icon(unsigned type,int angle,int present,int left_traffic) {
    int valid=present&&angle>=-180&&angle<=180;
    if(type>=MAN_TYPE_ROUNDABOUT_EXIT_1&&type<=MAN_TYPE_ROUNDABOUT_EXIT_19)
        return HUD_ICON_EXIT_1+type-MAN_TYPE_ROUNDABOUT_EXIT_1;

    switch(type) {
    case MAN_TYPE_FOLLOW_ROAD: return HUD_ICON_FOLLOW_ROAD;
    case MAN_TYPE_START_ROUTE: return HUD_ICON_START;
    case MAN_TYPE_KEEP_LEFT: return HUD_ICON_KEEP_LEFT;
    case MAN_TYPE_KEEP_RIGHT: return HUD_ICON_KEEP_RIGHT;
    case MAN_TYPE_LEFT_TURN_AT_END: return HUD_ICON_END_LEFT;
    case MAN_TYPE_RIGHT_TURN_AT_END: return HUD_ICON_END_RIGHT;
    case MAN_TYPE_ARRIVE_DESTINATION_LEFT: return HUD_ICON_ARRIVAL_LEFT;
    case MAN_TYPE_ARRIVE_DESTINATION_RIGHT: return HUD_ICON_ARRIVAL_RIGHT;
    case MAN_TYPE_ENTER_FERRY: return HUD_ICON_FERRY_ENTER;
    case MAN_TYPE_EXIT_FERRY: return HUD_ICON_FERRY_EXIT;
    case MAN_TYPE_CHANGE_FERRY: return HUD_ICON_FERRY_CHANGE;
    case MAN_TYPE_HIGHWAY_OFF_RAMP_LEFT: return HUD_ICON_OFF_RAMP_LEFT;
    case MAN_TYPE_HIGHWAY_OFF_RAMP_RIGHT: return HUD_ICON_OFF_RAMP_RIGHT;
    case MAN_TYPE_OFF_RAMP:
        if(!valid||!angle)return 9;
        return angle<0?HUD_ICON_OFF_RAMP_LEFT:HUD_ICON_OFF_RAMP_RIGHT;
    case MAN_TYPE_ON_RAMP:
        /* A turn toward the right joins the main road from its left. */
        if(!valid||!angle)return 9;
        return angle>0?HUD_ICON_MERGE_FROM_LEFT:HUD_ICON_MERGE_FROM_RIGHT;
    case MAN_TYPE_CHANGE_HIGHWAY_LEFT: return HUD_ICON_HIGHWAY_LEFT;
    case MAN_TYPE_CHANGE_HIGHWAY_RIGHT: return HUD_ICON_HIGHWAY_RIGHT;
    case MAN_TYPE_CHANGE_HIGHWAY:
        if(!valid)return HUD_ICON_HIGHWAY;
        if(angle < -22)return HUD_ICON_HIGHWAY_LEFT;
        if(angle > 22)return HUD_ICON_HIGHWAY_RIGHT;
        return HUD_ICON_HIGHWAY;
    case MAN_TYPE_U_TURN:
    case MAN_TYPE_START_ROUTE_WITH_U_TURN:
    case MAN_TYPE_U_TURN_WHEN_POSSIBLE:
        return (valid&&angle?angle>0:left_traffic)?HUD_ICON_UTURN_RIGHT:8;
    case MAN_TYPE_ENTER_ROUNDABOUT:
        return left_traffic?HUD_ICON_CW_ENTER:11;
    case MAN_TYPE_U_TURN_AT_ROUNDABOUT:
        return left_traffic?HUD_ICON_CW_RETURN:HUD_ICON_CCW_RETURN;
    case MAN_TYPE_EXIT_ROUNDABOUT:
        if(!valid)return 12;
        if(angle<=-158||angle>=158)return left_traffic?HUD_ICON_CW_RETURN:HUD_ICON_CCW_RETURN;
        if(angle < -22)return left_traffic?HUD_ICON_CW_EXIT_LEFT:HUD_ICON_CCW_EXIT_LEFT;
        if(angle > 22)return left_traffic?HUD_ICON_CW_EXIT_RIGHT:HUD_ICON_CCW_EXIT_RIGHT;
        return left_traffic?HUD_ICON_CW_EXIT_AHEAD:12;
    default: return hc_turn_icon_angle(type,angle,present);
    }
}
static unsigned direction(int degrees) {
    if(degrees < -180 || degrees > 180)return 0;
    if(degrees<=-158)return 8; /* left U turn */
    if(degrees>=158)return 16; /* right U turn */
    if(degrees < -22)return 2;
    if(degrees > 22)return 4;
    return 1;
}
static int shape(unsigned mask) {
    switch(mask) {
    case 1:return 0;case 2:return 1;case 3:return 2;case 4:return 3;
    case 5:return 4;case 8:return 5;case 6:return 6;case 7:return 7;
    case 16:return 8;case 9:return 9;case 17:return 10;
    case 10:return 11;case 20:return 12;
    default:return -1;
    }
}
API int hc_lane_pair(const int16_t *angles,size_t n,int primary,int status,uint8_t pair[2]) {
    if(!pair||(!angles&&n)||n>16||status<0||status>2)return 0;
    unsigned mask=0;
    for(size_t i=0;i<n;i++) {
        unsigned bit=direction(angles[i]);if(!bit)return 0;mask|=bit;
    }
    unsigned highlighted=direction(primary);
    if(status>0&&highlighted)mask|=highlighted;
    int back=shape(mask);if(back<0)return 0;
    pair[0]=(uint8_t)back;
    pair[1]=(uint8_t)(status==2&&highlighted?shape(highlighted):15);
    return 1;
}
static size_t safe_utf8(const char *s,uint8_t *out,size_t cap) {
    size_t i=0;
    while(s[i]&&i<cap) {
        unsigned char a=(unsigned char)s[i];size_t len;
        if(a<0x80)len=1;else if(a>=0xc2&&a<=0xdf)len=2;
        else if(a>=0xe0&&a<=0xef)len=3;else if(a>=0xf0&&a<=0xf4)len=4;else break;
        if(i+len>cap)break;
        int valid=1;
        for(size_t j=1;j<len;j++)if(!s[i+j]||((unsigned char)s[i+j]&0xc0)!=0x80){valid=0;break;}
        if(!valid)break;
        unsigned char b=(unsigned char)s[i+1];
        if((a==0xe0&&b<0xa0)||(a==0xed&&b>=0xa0)||(a==0xf0&&b<0x90)||(a==0xf4&&b>=0x90))break;
        memcpy(out+i,s+i,len);i+=len;
    }
    return i;
}
static size_t road_text(const hud_core *c, const rgd_maneuver_t *maneuver, uint8_t *out, size_t capacity) {
    const char *const text[] = {maneuver->description, maneuver->after_road_name, c->route.current_road};
    const char *const prefix[] = {"", "前方：", "当前："};
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); ++i) {
        const char *value = text[i] + strspn(text[i], " \t\r\n");
        size_t prefix_bytes = strlen(prefix[i]);
        if (!*value || prefix_bytes >= capacity)
            continue;
        size_t text_bytes = safe_utf8(value, out + prefix_bytes, capacity - prefix_bytes);
        if (text_bytes) {
            memcpy(out, prefix[i], prefix_bytes);
            return prefix_bytes + text_bytes;
        }
    }
    return 0;
}

static int render_lane_group(hud_core *c, const rgd_lane_guidance_t *group) {
    size_t count = group->lane_count;
    size_t first_position = group->lanes[0].position;
    for (size_t i = 1; i < count; ++i) {
        if (group->lanes[i].position < first_position)
            first_position = group->lanes[i].position;
    }

    const rgd_lane_t *ordered[MAX_LANE_GUIDANCE] = {0};
    for (size_t i = 0; i < count; ++i) {
        const rgd_lane_t *lane = &group->lanes[i];
        size_t slot = lane->position - first_position;
        if (slot >= count || ordered[slot])
            return 0;
        ordered[slot] = lane;
    }

    uint8_t pairs[MAX_LANE_GUIDANCE][2];
    for (size_t i = 0; i < count; ++i) {
        const rgd_lane_t *lane = ordered[i];
        if (!hc_lane_pair(lane->angles, lane->angle_count, lane->direction, lane->status, pairs[i]))
            return 0;
    }

    /* Preserve adjacency. Prefer the window with the most highlighted lanes,
     * then the most usable lanes, then the lowest starting index. */
    size_t shown = count < HC_MAX_DISPLAY_LANES ? count : HC_MAX_DISPLAY_LANES;
    size_t best_start = 0;
    unsigned best_score = 0;
    for (size_t start = 0; start + shown <= count; ++start) {
        unsigned score = 0;
        for (size_t i = start; i < start + shown; ++i) {
            score += ordered[i]->status > 0;
            score += (pairs[i][1] != 15) * (HC_MAX_DISPLAY_LANES + 1);
        }
        if (score > best_score) {
            best_start = start;
            best_score = score;
        }
    }

    /* All three commands are limited to the six slots in the HUD receiver. */
    uint8_t simple[2 + HC_MAX_DISPLAY_LANES] = {0x11, (uint8_t)shown};
    uint8_t v1[1 + 2 * HC_MAX_DISPLAY_LANES] = {0x12};
    uint8_t v2[1 + 2 * HC_MAX_DISPLAY_LANES] = {0x15};
    for (size_t i = 0; i < shown; ++i) {
        const uint8_t *pair = pairs[best_start + i];
        simple[i + 2] = pair[1] != 15;
        memcpy(v1 + 1 + 2 * i, pair, 2);
        memcpy(v2 + 1 + 2 * i, pair, 2);
    }
    enqueue(c, simple, shown + 2);
    enqueue(c, v1, 2 * shown + 1);
    enqueue(c, v2, 2 * shown + 1);
    return 1;
}

static void render(hud_core *c,unsigned dirty) {
    if(c->phase!=HC_READY)return;
    if(!c->active){clear_nav(c);return;}
    man_slot *m=maneuver(c,current_index(c),0);
    int icon=-1;
    if(m&&(m->v.present&RGD_MAN_TYPE)){
        int angle_present=(m->v.present&RGD_MAN_EXIT_ANGLE)!=0;
        int left_traffic=(m->v.present&RGD_MAN_DRIVING_SIDE)&&m->v.driving_side==DRIVING_SIDE_LEFT;
        icon=c->expanded_icons?expanded_turn_icon(m->v.maneuver_type,m->v.exit_angle,angle_present,left_traffic):
            hc_turn_icon_angle(m->v.maneuver_type,m->v.exit_angle,angle_present);
    }
    if(icon<0||!(c->route.present&RGD_UPD_DIST_TO_MANEUVER)){clear_nav(c);return;}
    if(!c->display_on) {
        uint8_t start[]={0xe0,1};enqueue(c,start,2);c->display_on=1;dirty=3;
        const uint8_t speed[]={1,0,0,0,0},cam[]={0x13,0,0,0,0},old[]={0x10,0,0,0,0};
        enqueue(c,speed,5);enqueue(c,cam,5);enqueue(c,old,5);
    }
    if(dirty&1) {
        uint32_t dist=c->route.dist_to_maneuver;
        uint8_t turn[]={0,(uint8_t)icon,(uint8_t)dist,(uint8_t)(dist>>8),(uint8_t)(dist>>16),(uint8_t)(dist>>24)};
        enqueue(c,turn,sizeof(turn));
        uint8_t road[122]={5};
        size_t text_n=road_text(c,&m->v,road+1,120);
        enqueue(c,road,text_n?text_n+1:2); /* explicit NUL when the line is empty */
    }
    if(!(dirty&2))return;
    lane_slot *ls=lane(c,c->route.lane_guidance_index,0);
    if (!(c->route.present & RGD_UPD_LANE_INDEX) || !c->route.lane_guidance_showing ||
        !ls || !ls->v.lane_complete || !ls->v.lane_count || !render_lane_group(c, &ls->v))
        clear_lanes(c);
}

API hud_core *hc_create(uint16_t component) {
    hud_core *c=calloc(1,sizeof(*c));if(c){c->component=component;c->available=1;}return c;
}
API void hc_transport_reset(hud_core *c,int available) {
    if(!c)return;
    c->available=!!available;c->phase=HC_IDLE;c->attempts=0;c->demand=0;
    c->generation++;c->deadline=0;c->write_deadline=0;drop_queue(c);
}
API void hc_destroy(hud_core *c){free(c);}
API int hc_phase(const hud_core *c){return c?c->phase:-1;}
API int hc_attempts(const hud_core *c){return c?c->attempts:0;}
API uint32_t hc_generation(const hud_core *c){return c?c->generation:0;}
API unsigned hc_session_end(hud_core *c) {
    if(!c)return 0;
    memset(&c->route,0,sizeof(c->route));memset(c->mans,0,sizeof(c->mans));memset(c->lanes,0,sizeof(c->lanes));
    c->active=0;c->demand=0;c->distance_received_ms=0;
    if(c->phase==HC_READY){clear_nav(c);return 0;}
    unsigned action=c->phase==HC_SCANNING?HC_STOP_SCAN:
        (c->phase==HC_CONNECTING||c->phase==HC_DISCOVERING?HC_CLOSE:0);
    c->phase=HC_IDLE;c->generation++;drop_queue(c);return action;
}
#define MERGE(dst,src,bit,field) do { if((src).present&(bit))memcpy(&(dst).field,&(src).field,sizeof((src).field)); } while(0)
static unsigned hc_input_received(hud_core *c,const uint8_t *p,size_t n,uint64_t now,uint64_t received_ms) {
    if(!c||!p||n<6||n>65535)return 0;
    uint16_t id=read_be16(p+4);int trigger=0;unsigned dirty=0;
    if(id==0x5201) {
        rgd_update_t u;if(!rgd_parse_update(p,n,&u)||!accepts(c,u.component_ids,u.component_count))return 0;
        if((u.present&RGD_UPD_ROUTE_STATE)&&u.route_state>6)return 0;
        if((u.present&RGD_UPD_ROUTE_STATE)&&(u.route_state==0||u.route_state==2))return hc_session_end(c);
        if((u.present&RGD_UPD_ROUTE_STATE)&&(u.route_state==1||u.route_state==5||u.route_state==6))c->active=1;
        /* Distances must never leak across a new current-maneuver selection. */
        if((u.present&RGD_UPD_MANEUVER_LIST)&&(!u.maneuver_list_count||!c->route.maneuver_list_count||u.maneuver_list[0]!=c->route.maneuver_list[0])){
            c->route.present&=~RGD_UPD_DIST_TO_MANEUVER;
            c->distance_received_ms=0;
        }
        MERGE(c->route,u,RGD_UPD_ROUTE_STATE,route_state);
        MERGE(c->route,u,RGD_UPD_DIST_TO_MANEUVER,dist_to_maneuver);
        MERGE(c->route,u,RGD_UPD_CURRENT_ROAD,current_road);
        MERGE(c->route,u,RGD_UPD_MANEUVER_LIST,maneuver_list);
        MERGE(c->route,u,RGD_UPD_MANEUVER_LIST,maneuver_list_count);
        MERGE(c->route,u,RGD_UPD_LANE_INDEX,lane_guidance_index);
        MERGE(c->route,u,RGD_UPD_LANE_SHOWING,lane_guidance_showing);
        c->route.present|=u.present;
        if(!guidance_route(c))c->distance_received_ms=0;
        else if(u.present&RGD_UPD_DIST_TO_MANEUVER)c->distance_received_ms=received_ms;
        if(u.present&(RGD_UPD_ROUTE_STATE|RGD_UPD_DIST_TO_MANEUVER|RGD_UPD_MANEUVER_LIST|RGD_UPD_CURRENT_ROAD))dirty|=1;
        if(u.present&(RGD_UPD_LANE_INDEX|RGD_UPD_LANE_SHOWING))dirty|=2;
        trigger=dirty!=0;
    } else if(id==0x5202) {
        rgd_maneuver_t u;if(!rgd_parse_maneuver(p,n,&u)||!(u.present&RGD_MAN_INDEX)||!accepts(c,u.component_ids,u.component_count))return 0;
        man_slot *s=maneuver(c,u.index,1);if(!s)return 0;
        if((u.present&s->v.present&RGD_MAN_TYPE)&&u.maneuver_type!=s->v.maneuver_type){
            if(u.index==current_index(c))c->distance_received_ms=0;
            s->v.description[0]=0;
            s->v.after_road_name[0]=0;
            s->v.present&=~(RGD_MAN_DESCRIPTION|RGD_MAN_AFTER_ROAD);
        }
        MERGE(s->v,u,RGD_MAN_TYPE,maneuver_type);MERGE(s->v,u,RGD_MAN_AFTER_ROAD,after_road_name);
        MERGE(s->v,u,RGD_MAN_DESCRIPTION,description);
        MERGE(s->v,u,RGD_MAN_EXIT_ANGLE,exit_angle);
        MERGE(s->v,u,RGD_MAN_DRIVING_SIDE,driving_side);s->v.present|=u.present;s->age=++c->age;
        uint64_t drawing_fields=RGD_MAN_TYPE|RGD_MAN_DESCRIPTION|RGD_MAN_AFTER_ROAD|RGD_MAN_EXIT_ANGLE;
        if(c->expanded_icons)drawing_fields|=RGD_MAN_DRIVING_SIDE;
        trigger=u.index==current_index(c)&&(u.present&drawing_fields);
        dirty=1;
    } else if(id==0x5204) {
        rgd_lane_guidance_t u;if(!rgd_parse_lane_guidance(p,n,&u)||!(u.present&RGD_LANE_GUIDANCE_INDEX)||!accepts(c,u.component_ids,u.component_count))return 0;
        lane_slot *s=lane(c,u.lane_guidance_index,1);if(!s)return 0;
        if(u.present&RGD_LANE_INFORMATIONS){s->v=u;}else s->v.present|=u.present;
        s->age=++c->age;
        trigger=c->route.lane_guidance_showing&&u.lane_guidance_index==c->route.lane_guidance_index&&(u.present&RGD_LANE_INFORMATIONS);
        dirty=2;
    } else return 0;
    if(!trigger||!c->active)return 0;
    unsigned action=demand(c,now);render(c,dirty);return action;
}
API unsigned hc_input(hud_core *c,const uint8_t *p,size_t n,uint64_t now) {
    return hc_input_received(c,p,n,now,now);
}
API unsigned hc_found(hud_core *c,uint32_t g,uint64_t now) {
    if(!c||c->phase!=HC_SCANNING||g!=c->generation)return 0;
    c->phase=HC_CONNECTING;c->deadline=now+20000;return HC_STOP_SCAN|HC_CONNECT;
}
API unsigned hc_connected(hud_core *c,uint32_t g,uint64_t now) {
    if(!c||c->phase!=HC_CONNECTING||g!=c->generation)return 0;
    c->phase=HC_DISCOVERING;c->deadline=now+12000;return HC_DISCOVER;
}
API unsigned hc_ready(hud_core *c,uint32_t g,uint64_t now) {
    if(!c||c->phase!=HC_DISCOVERING||g!=c->generation)return 0;
    c->phase=HC_READY;drop_queue(c);c->next_write=now;render(c,3);return 0;
}
API unsigned hc_disconnected(hud_core *c,uint32_t g) {
    if(!c||g!=c->generation||c->phase!=HC_READY)return 0;
    c->phase=HC_IDLE;c->generation++;drop_queue(c);return HC_CLOSE;
}
API unsigned hc_failed(hud_core *c,uint32_t g,uint64_t now) {
    if(!c||g!=c->generation||(c->phase!=HC_SCANNING&&c->phase!=HC_CONNECTING&&c->phase!=HC_DISCOVERING))return 0;
    unsigned action=c->phase==HC_SCANNING?HC_STOP_SCAN:HC_CLOSE;
    c->generation++;c->phase=c->demand&&c->attempts<12?HC_WAITING:HC_IDLE;
    c->deadline=now+5000;return action;
}
API unsigned hc_tick(hud_core *c,uint64_t now) {
    if(!c)return 0;
    if(c->phase==HC_READY&&(c->in_flight||c->write_busy)&&now>=c->write_deadline)return hc_disconnected(c,c->generation);
    if(c->phase==HC_WAITING&&now>=c->deadline)return scan(c,now);
    if((c->phase==HC_SCANNING||c->phase==HC_CONNECTING||c->phase==HC_DISCOVERING)&&now>=c->deadline) {
        return hc_failed(c,c->generation,now);
    }
    return 0;
}
API size_t hc_next_chunk(hud_core *c,uint64_t now,uint8_t *out,size_t cap) {
    if(!c||!out||!cap||c->phase!=HC_READY||c->in_flight||now<c->next_write)return 0;
    if(!c->current.n) {
        if(!c->queued)return 0;c->current=c->pending[0];c->queued--;
        memmove(c->pending,c->pending+1,c->queued*sizeof(frame));c->offset=0;
    }
    size_t n=c->current.n-c->offset;if(n>cap)n=cap;
    memcpy(out,c->current.bytes+c->offset,n);c->in_flight=n;
    if(!c->write_busy)c->write_deadline=now+2000;
    return n;
}
API unsigned hc_written(hud_core *c,uint32_t g,int success,uint64_t now) {
    if(!c||g!=c->generation||c->phase!=HC_READY||!c->in_flight)return 0;
    if(!success)return hc_disconnected(c,g);
    c->offset+=c->in_flight;c->in_flight=0;c->next_write=now+16;c->write_busy=0;
    if(c->offset==c->current.n){c->current.n=0;c->offset=0;}
    return 0;
}
API unsigned hc_write_busy(hud_core *c,uint32_t g,uint64_t now) {
    if(!c||g!=c->generation||c->phase!=HC_READY||!c->in_flight)return 0;
    if(now>=c->write_deadline)return hc_disconnected(c,g);
    c->in_flight=0;c->next_write=now+16;c->write_busy=1;return 0;
}
API size_t hc_component_tlv(uint8_t *out,size_t cap,uint16_t component) {
    return rgd_build_component_tlv(out,cap,component);
}
API size_t hc_subscription(uint8_t *out,size_t cap,uint16_t component) {
    if(!out||cap<24)return 0;
    const uint8_t prefix[]={0x40,0x40,0,24,0x52,0,0,6,0,0};
    const uint8_t options[]={0,4,0,1,0,4,0,2,0,4,0,3};
    memcpy(out,prefix,10);write_be16(out+10,component);memcpy(out+12,options,12);return 24;
}
