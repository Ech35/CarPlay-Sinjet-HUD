#include "core_adapter.h"
#include "core/hud_core.c"
hud_core *u5_core_create(uint16_t component){return hc_create(component);}
int u5_core_set_expanded_icons(hud_core *c,int enabled){
    if(!c||c->expanded_icons==!!enabled)return 0;
    c->expanded_icons=!!enabled;
    return 1;
}
size_t u5_core_encode_frame(uint8_t *out,size_t capacity,const uint8_t *payload,size_t size){
    frame f;
    if(capacity<5||size>capacity-5||!pack(&f,payload,size))return 0;
    memcpy(out,f.bytes,f.n);
    return f.n;
}
unsigned u5_core_session_end(hud_core *c){return hc_session_end(c);}
void u5_core_clear_display(hud_core *c){if(c&&c->phase==HC_READY)clear_nav(c);}
unsigned u5_core_input(hud_core *c,const uint8_t *p,size_t n,uint64_t now,uint64_t received_ms){
    return hc_input_received(c,p,n,now,received_ms);
}
void u5_core_selection(hud_core *c,struct U5Selection *out){
    *out=(struct U5Selection){65535,65535,255,255};if(!c)return;
    int index=current_index(c);if(index<0)return;
    out->raw_index=out->selected_index=(unsigned)index;
    man_slot *m=maneuver(c,index,0);
    if(m&&(m->v.present&RGD_MAN_TYPE))out->raw_type=out->selected_type=m->v.maneuver_type;
}
int u5_core_refresh(hud_core *c){
    if(!c||c->phase!=HC_READY||!c->active||!c->display_on||!guidance_route(c))return 0;
    render(c,3);return c->display_on;
}
int u5_core_has_guidance(hud_core *c){
    if(!c||!c->active||!guidance_route(c)||!(c->route.present&RGD_UPD_DIST_TO_MANEUVER))return 0;
    man_slot *m=maneuver(c,current_index(c),0);
    return m&&(m->v.present&RGD_MAN_TYPE);
}
int u5_core_resume(hud_core *c,uint64_t now,uint64_t max_age_ms){
    if(!c||!c->available)return 0;
    /* A lane or maneuver update cannot refresh an old distance. Keep the
     * route and maneuver cache so a new distance can still arrive as a delta. */
    if(!c->distance_received_ms||now<c->distance_received_ms||now-c->distance_received_ms>max_age_ms){
        c->route.present&=~RGD_UPD_DIST_TO_MANEUVER;
        return 0;
    }
    if(!u5_core_has_guidance(c))return 0;
    demand(c,now);
    return 1;
}
