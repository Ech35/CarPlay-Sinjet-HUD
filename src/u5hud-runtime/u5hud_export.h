#include "u5hud_parse.h"

static const char *event_name(unsigned kind) {
    static const char *names[] = {"state", "initialized", "logging_enabled", "logging_disabled", "cleared",
        "session_begin", "identify", "identify_tx_messages", "identify_rx_messages", "identification_accepted",
        "subscription_attempt", "subscription_return", "received_start_identification", "received_identification_accepted",
        "identification_rejected", "received_cancel_identification", "navigation_update", "navigation_maneuver",
        "navigation_lanes", "gps_requested", "gps_stopped", "gps_received", "frame_pending", "frame_error", "raw_continuation",
        "native_success_callback_completed", "accessory_transport_tx", "phone_link_rx", "headunit_to_phone_entry",
        "navigation_on_other_channel", "native_pass_state_change", "identification_dispatch_completed",
        "native_pass_state_applied"};
    return kind < sizeof(names)/sizeof(names[0]) ? names[kind] : "unknown_event";
}
static const char *reason_name(unsigned reason) {
    static const char *names[] = {"none", "invalid_length", "short_tlv_header", "invalid_tlv_length",
        "identify_buffer_capacity", "component_id_collision", "missing_message_list", "duplicate_message_list",
        "fallback_after_rejection", "stock_component_present", "component_added", "reference_value_shape",
        "diagnostic_frame_limit", "phone_provided_no_rejection_details", "waiting_for_more_bytes"};
    return reason < sizeof(names)/sizeof(names[0]) ? names[reason] : "unknown_reason";
}
static const char *subscription_source(unsigned trigger) {
    return trigger == 1 ? "success_callback_return" : trigger == 2 ? "accepted_dispatch_end" :
           trigger == 3 ? "native_pass_applied" : "none";
}
static const char *parse_name(unsigned result) {
    static const char *names[] = {"not_applicable", "tlv_ok", "bad_frame_header", "incomplete", "malformed_tlv",
        "reference_shape_warning", "diagnostic_size_limit"};
    return result < sizeof(names)/sizeof(names[0]) ? names[result] : "unknown_parse_status";
}
static const char *reject_name(unsigned id) {
    static const char *names[] = {"Name", "ModelIdentifier", "Manufacturer", "SerialNumber", "FirmwareVersion",
        "HardwareVersion", "MessagesSentByAccessory", "MessagesReceivedFromDevice", "PowerSourceType",
        "MaximumCurrentDrawnFromDevice", "SupportedExternalAccessoryProtocol", "PreferredAppBundleSeedIdentifier",
        "CurrentLanguage", "SupportedLanguage", "SerialTransportComponent", "USBDeviceTransportComponent",
        "USBHostTransportComponent", "BluetoothTransportComponent", "iAP2HIDComponent", "USBHostHIDComponent"};
    return id < sizeof(names)/sizeof(names[0]) ? names[id] : id == 30 ? "RouteGuidanceDisplayComponent_reference" : "unknown_parameter";
}
static const char *field_name(unsigned mid, unsigned id) {
    static const char *route[] = {"component_ids", "route_state", "maneuver_state", "current_road", "destination",
        "eta", "time_remaining", "distance_remaining", "distance_text", "distance_units", "distance_to_maneuver",
        "maneuver_distance_text", "maneuver_distance_units", "maneuver_indices", "maneuver_count", "visible_in_app",
        "lane_guidance_index", "lane_guidance_total", "lane_guidance_showing", "source_name", "source_supports_guidance"};
    static const char *maneuver[] = {"component_ids", "maneuver_index", "maneuver_description", "maneuver_type",
        "after_road", "distance_between", "distance_text", "distance_units", "driving_side", "junction_type",
        "junction_angles", "exit_angle", "linked_lane_guidance_index", "exit_information"};
    static const char *lane[] = {"component_ids", "lane_guidance_index", "lane_informations", "lane_description"};
    static const char *inner[] = {"lane_index", "lane_status", "lane_angles", "lane_angle_highlight"};
    static const char *component[] = {"component_id", "component_name", "max_current_road_length", "max_destination_length",
        "max_after_road_length", "max_maneuver_description_length", "max_maneuver_storage", "max_lane_description_length", "max_lane_storage"};
    if (mid == 0x1d03 || mid == 0x1d01) return reject_name(id);
    if (mid == 0x5201 && id < sizeof(route)/sizeof(route[0])) return route[id];
    if (mid == 0x5202 && id < sizeof(maneuver)/sizeof(maneuver[0])) return maneuver[id];
    if (mid == 0x5204 && id < sizeof(lane)/sizeof(lane[0])) return lane[id];
    if (mid == 0x15204 && id < sizeof(inner)/sizeof(inner[0])) return inner[id];
    if (mid == 0x1001e && id < sizeof(component)/sizeof(component[0])) return component[id];
    if (mid == 0x5200) return id == 0 ? "component_id" : id == 1 ? "request_source_name" :
        id == 2 ? "request_source_supports_guidance" : id == 3 ? "request_exit_information" : "unknown_field";
    if (mid == 0xfffa) return id == 0 ? "minimum_interval_ms" : id == 1 ? "allows_GPGGA" :
        id == 2 ? "allows_GPRMC" : id == 3 ? "allows_GPGSV" : "unknown_field";
    return "unknown_field";
}

static void print_text(const unsigned char *p, unsigned n) {
    putchar('"');
    unsigned limit = n < 512 ? n : 512;
    for (unsigned i = 0; i < limit && p[i];) {
        unsigned c = p[i], bytes = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
            c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        int valid = bytes && bytes <= limit - i;
        for (unsigned k = 1; k < bytes && valid; ++k) valid = (p[i+k] & 0xc0) == 0x80;
        if (valid && bytes == 3) valid = !(c == 0xe0 && p[i+1] < 0xa0) && !(c == 0xed && p[i+1] >= 0xa0);
        if (valid && bytes == 4) valid = !(c == 0xf0 && p[i+1] < 0x90) && !(c == 0xf4 && p[i+1] >= 0x90);
        if (!valid) { printf("\\x%02x", c); ++i; continue; }
        if (c == '\\' || c == '"') { putchar('\\'); putchar(c); }
        else if (c < 32 || c == 127) printf("\\x%02x", c);
        else fwrite(p+i, 1, bytes, stdout);
        i += bytes;
    }
    putchar('"');
    if (n > limit) printf(" [text_preview_limited=%u]", limit);
}

static void dump_tlvs(unsigned mid, const unsigned char *p, unsigned n, unsigned base,
                      unsigned depth, unsigned *budget) {
    for (unsigned off = 0; off < n && *budget;) {
        if (n - off < 4) { printf("  tlv_not_fully_available offset=%u bytes=%u\n", base+off, n-off); return; }
        unsigned len = u5hud_be16(p+off), id = u5hud_be16(p+off+2);
        if (len < 4 || len > n-off) {
            printf("  tlv_unavailable_or_invalid offset=%u id=0x%04x declared=%u available=%u\n", base+off, id, len, n-off); return;
        }
        --*budget;
        unsigned bytes = len-4, type = u5hud_field_type(mid, id);
        const unsigned char *value = p+off+4;
        if (mid == 0x1d01) type = id == 6 || id == 7 ? U5_U16_LIST : U5_BLOB;
        if (mid == 0x15204) type = id == 0 ? U5_U16 : id == 1 ? U5_U8 : id == 2 ? U5_I16_LIST : id == 3 ? U5_I16 : U5_BLOB;
        if (mid == 0x15204 && id == 0 && bytes > 2 && u5hud_sequence_valid(value,bytes)) type = U5_LANE_GROUP;
        if (mid == 0x1001e) type = id == 1 ? U5_TEXT : id <= 8 ? U5_U16 : U5_BLOB;
        printf("  %*stlv offset=%u id=0x%04x field=%s value_bytes=%u", (int)depth*2, "", base+off, id, field_name(mid,id), bytes);
        if (mid == 0x1d03 && id != 6 && id != 7) printf(" rejected_parameter=yes reason_detail=unspecified");
        if (type == U5_U8 && bytes >= 1) printf(" value=%u", value[0]);
        else if (type == U5_U16 && bytes >= 2) printf(" value=%u", u5hud_be16(value));
        else if (type == U5_I16 && bytes >= 2) printf(" value=%d", (int)(int16_t)u5hud_be16(value));
        else if (type == U5_U32 && bytes >= 4) printf(" value=%u", u5hud_be32(value));
        else if (type == U5_U64 && bytes >= 8) printf(" value=%llu", (unsigned long long)((uint64_t)u5hud_be32(value)<<32 | u5hud_be32(value+4)));
        else if (type == U5_TEXT) { printf(" value="); print_text(value, bytes); }
        else if (type == U5_FLAG) printf(" flag_present=yes");
        else if (type == U5_U16_LIST || type == U5_I16_LIST) {
            printf(mid == 0x1d03 ? " unsupported_message_ids=[" : " values=[");
            unsigned count = bytes/2, shown = count < 64 ? count : 64;
            for (unsigned i = 0; i < shown; ++i) {
                if (i) putchar(',');
                if (type == U5_I16_LIST) printf("%d", (int)(int16_t)u5hud_be16(value+i*2));
                else printf("0x%04x", u5hud_be16(value+i*2));
            }
            printf("]");
            if (shown < count) printf(" displayed=%u total=%u", shown, count);
            if (bytes & 1) printf(" trailing_byte=yes");
        }
        putchar('\n');
        if (depth < 3 && type == U5_LANE_GROUP)
            dump_tlvs(0x15204, value, bytes, base+off+4, depth+1, budget);
        else if (depth < 3 && mid == 0x1d01 && id == 30)
            dump_tlvs(0x1001e, value, bytes, base+off+4, depth+1, budget);
        off += len;
    }
    if (!*budget) printf("  decoded_field_limit=128; raw_capture_retained=yes\n");
}

static void dump_trace(const unsigned char *snapshot) {
    const struct U5HudShared *header = (const struct U5HudShared *)snapshot;
    static const char *directions[] = {"phone_link_rx", "accessory_transport_tx", "headunit_to_phone_entry"};
    printf("\n[traffic_summary]\nunit=observed_chunks_not_complete_messages\n"
           "message_0x10000=unframed_or_fragment\nphone_receipt_confirmation=not_available\n");
    for (unsigned d = 0; d < 3; ++d) {
        printf("direction=%s chunks=%u bytes=%u last_uptime_seconds=%u last_message=0x%04x unlisted_chunks=%u\n",
               directions[d],header->traffic_chunks[d],header->traffic_bytes[d],header->traffic_last_seconds[d],
               header->traffic_last_message[d],header->traffic_unlisted[d]);
        for (unsigned j = 0; j < U5HUD_MESSAGE_SLOTS; ++j) {
            const struct U5HudMessageCount *entry = &header->messages[d][j];
            if (entry->chunks) printf("  channel=%u first_message=0x%04x chunks=%u bytes=%u last_uptime_seconds=%u\n",
                                     entry->channel,entry->message_id,entry->chunks,entry->bytes,entry->last_seconds);
        }
    }
    printf("[end_traffic_summary]\n");
    unsigned count = header->trace_count < U5HUD_EVENT_CAPACITY ? header->trace_count : U5HUD_EVENT_CAPACITY;
    if (header->trace_next < count) { printf("timeline=invalid_header\n"); return; }
    unsigned first = header->trace_next-count+1;
    printf("\n[timeline]\nclock=monotonic_uptime\nsemantic_schema=local_rgi_reference_v1\n"
           "rejection_schema=provided_interface_R2_parameter_ids\nrecords=%u capacity=%u overwritten=%u dropped=%u\n",
           count,U5HUD_EVENT_CAPACITY,header->trace_overwritten,header->trace_dropped);
    unsigned char frame[U5HUD_FRAME_BYTES];
    for (unsigned i = 0; i < count; ++i) {
        const struct U5HudEvent *e = u5hud_event_at((void *)snapshot, first+i);
        if (e->sequence != first+i || e->raw_length > U5HUD_RAW_BYTES) {
            printf("event_invalid sequence_expected=%u\n", first+i); continue;
        }
        printf("\n[%u.%03u] seq=%u session=%u event=%s message=0x%04x group=%u\n"
               "  wire_bytes=%u captured_chunk=%u raw_offset=%u flags=0x%x parse=%s reason=%s error_offset=%u tlvs=%u unknown=%u known_mask=0x%08x\n",
               e->seconds,e->milliseconds,e->sequence,e->session,event_name(e->kind),e->message_id,e->group,
               e->wire_length,e->raw_length,e->raw_offset,e->flags,parse_name(e->result),reason_name(e->reason),
               e->error_offset,e->tlv_count,e->unknown_count,e->known_mask);
        if (e->kind == U5_IDENTIFY)
            printf("  identify_mode=%u original_bytes=%u resulting_bytes=%u buffer_capacity=%u\n",
                   e->values[0],e->values[1],e->values[2],e->values[3]);
        if (e->kind == U5_SUBSCRIBE_ATTEMPT) printf("  attempted_component_id=0x%04x delivery_confirmation=not_available\n", e->auxiliary);
        if (e->kind == U5_SUBSCRIBE_RESULT) printf("  native_send_return=%d delivery_confirmation=not_available\n", (int)e->auxiliary);
        if (e->kind == U5_LINK_RX || e->kind == U5_HU_TX || e->kind == U5_OTHER_CHANNEL_NAV)
            printf("  observed_channel=%u\n",e->auxiliary);
        if (e->kind == U5_ACCEPTED || e->kind == U5_ACCEPTED_DISPATCH_DONE || e->kind == U5_NATIVE_PASS_APPLIED ||
            e->kind == U5_PROTOCOL_READY || e->kind == U5_TRANSPORT_TX ||
            e->kind == U5_SUBSCRIBE_ATTEMPT || e->kind == U5_SUBSCRIBE_RESULT)
            printf("  callback_completed=%u native_pass_state=%u native_return_state_at_observation=%u "
                   "transport_present=%u link_present=%u session_count=%u type_zero_sessions_in_first_16=%u "
                   "context_channel=%u local_handoff_only=yes\n",
                   e->values[1],e->values[2],e->values[3],e->values[4],e->values[5],e->values[6],e->values[7],e->values[8]);
        if (e->kind == U5_ACCEPTED || e->kind == U5_ACCEPTED_DISPATCH_DONE || e->kind == U5_NATIVE_PASS_APPLIED ||
            e->kind == U5_PROTOCOL_READY || e->kind == U5_SUBSCRIBE_ATTEMPT || e->kind == U5_SUBSCRIBE_RESULT ||
            e->kind == U5_TRANSPORT_TX)
            printf("  identification_dispatch_completed=%u native_success_callback_present=%s subscription_trigger=%s\n",
                   e->values[11],e->values[13] ? (e->values[14] ? "yes" : "no") : "unknown",
                   subscription_source(e->values[12]));
        if (e->kind == U5_TRANSPORT_TX) printf("  observed_channel=%u\n",e->auxiliary);
        if (e->kind == U5_NATIVE_PASS_STATE) printf("  requested_native_pass_state=%u\n",e->auxiliary);
        if (e->kind == U5_GPS_RECEIVED) printf("  gps_fix=%s\n",e->auxiliary==1 ? "valid" : e->auxiliary==2 ? "invalid" : "unknown");
        if (e->text[0]) { printf("  first_text_preview="); print_text(e->text,sizeof(e->text)); putchar('\n'); }
        if (e->raw_length) {
            char hex[U5HUD_RAW_BYTES*2+1];
            static const char digits[] = "0123456789abcdef";
            for (unsigned b=0;b<e->raw_length;++b) { hex[b*2]=digits[e->raw[b]>>4];hex[b*2+1]=digits[e->raw[b]&15]; }
            hex[e->raw_length*2]=0;
            printf("  raw_hex=%s\n",hex);
        }
        if (e->kind == U5_RAW_CONTINUATION) {
            if (e->group < first) printf("  parent_record_overwritten=yes\n");
            continue;
        }
        if (!e->raw_length) continue;
        memcpy(frame,e->raw,e->raw_length);
        unsigned collected=e->raw_length;
        for (unsigned j=i+1;j<count;++j) {
            const struct U5HudEvent *next=u5hud_event_at((void *)snapshot,first+j);
            if (next->sequence!=first+j || next->kind!=U5_RAW_CONTINUATION || next->group!=e->group ||
                next->raw_offset!=collected || next->raw_length>U5HUD_RAW_BYTES || next->raw_length>sizeof(frame)-collected) break;
            memcpy(frame+collected,next->raw,next->raw_length); collected+=next->raw_length;
        }
        printf("  sample_complete=%s sample_bytes=%u\n", collected==e->wire_length ? "yes" : "no",collected);
        unsigned budget=128;
        if (e->flags & U5_RAW_SELECTED_TLV) dump_tlvs(0x1d01,frame,collected,0,0,&budget);
        else if (collected>=6 && u5hud_be16(frame)==0x4040)
            dump_tlvs(e->message_id,frame+6,collected-6,6,0,&budget);
    }
    printf("\n[end_timeline]\n");
}

static unsigned char *trace_snapshot(const struct U5HudShared *shared) {
    if (!shared) return NULL;
    unsigned char *copy=malloc(U5HUD_STATE_BYTES);
    if (!copy) return NULL;
    const struct timespec pause={0,1000000};
    for (unsigned attempt=0;attempt<8;++attempt) {
        unsigned before=__atomic_load_n(&shared->sequence,__ATOMIC_ACQUIRE);
        if (before&1) { nanosleep(&pause,NULL); continue; }
        memcpy(copy,shared,U5HUD_STATE_BYTES);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before==__atomic_load_n(&shared->sequence,__ATOMIC_ACQUIRE)) return copy;
        nanosleep(&pause,NULL);
    }
    free(copy);return NULL;
}
