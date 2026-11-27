#ifndef U5_CORE_ADAPTER_H
#define U5_CORE_ADAPTER_H
#include "core/hud_core.h"
struct U5Selection {unsigned raw_index,selected_index,raw_type,selected_type;};
API hud_core *u5_core_create(uint16_t component);
API int u5_core_set_expanded_icons(hud_core *c,int enabled);
API size_t u5_core_encode_frame(uint8_t *out,size_t capacity,const uint8_t *payload,size_t size);
API unsigned u5_core_input(hud_core *c,const uint8_t *p,size_t n,uint64_t now,uint64_t received_ms);
API unsigned u5_core_session_end(hud_core *c);
API void u5_core_clear_display(hud_core *c);
API int u5_core_refresh(hud_core *c);
API int u5_core_has_guidance(hud_core *c);
API int u5_core_resume(hud_core *c,uint64_t now,uint64_t max_age_ms);
API void u5_core_selection(hud_core *c,struct U5Selection *out);
#endif
