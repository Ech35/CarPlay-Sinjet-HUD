#include "../u5hud-runtime/u5hud_parse.h"

static void diagnostic_transport_state(struct ProbeState *s, struct U5HudEvent *e) {
    void *owner = s->ctx ? ptr(s->ctx, 0) : 0;
    e->values[0] = e->auxiliary;
    e->values[1] = s->protocol_ready;
    e->values[11] = s->accepted_dispatch_complete;
    e->values[12] = s->subscription_trigger;
    e->values[13] = s->callback_known;
    e->values[14] = s->callback_present;
    if (!owner) return;
    e->values[2] = *(unsigned *)((char *)owner + 0x144);
    e->values[3] = *(unsigned *)((char *)owner + 0x1c);
    void *transport = ptr(owner, 0xd0);
    e->values[4] = !!transport;
    void *link = transport ? ptr(transport, 12) : 0;
    e->values[5] = !!link;
    if (link) {
        unsigned count = ((const unsigned char *)link)[0x142];
        e->values[6] = count;
        /* Same type-zero session lookup used by the native send path. The
         * diagnostic scan is bounded even if this count is corrupt. */
        for (unsigned i = 0; i < count && i < 16; ++i)
            e->values[7] += ((const unsigned char *)link)[0x147 + i * 3] == 0;
    }
    e->values[8] = ((const unsigned char *)s->ctx)[0x20];
    e->values[9] = s->native_pass_known;
    e->values[10] = s->native_pass_enabled;
}

static void diagnostic_emit(struct ProbeState *s, unsigned kind, unsigned mid,
                            const unsigned char *p, unsigned n, unsigned reason,
                            unsigned auxiliary, unsigned flags) {
    struct U5HudShared *shared = diagnostic_mapping(s);
    if (!shared) return;
    unsigned enabled = __atomic_load_n(&shared->enabled, __ATOMIC_ACQUIRE);
    unsigned changed = shared->session_pid != s->pid || shared->session_accepted != s->accepted ||
                       shared->protocol_ready != s->protocol_ready ||
                       shared->native_pass_known != s->native_pass_known ||
                       shared->native_pass_enabled != s->native_pass_enabled ||
                       shared->accepted_dispatch_complete != s->accepted_dispatch_complete ||
                       shared->callback_known != s->callback_known || shared->callback_present != s->callback_present ||
                       shared->subscription_trigger != s->subscription_trigger || shared->subscription_attempted != s->subscribe;
    if (!enabled && !changed && !s->new_session && s->trace_session) return;
    int now[2] = {0, 0};
    if (enabled || (changed && s->accepted)) linux_call(263, 1, (long)now, 0, 0, 0, 0);
    struct U5HudParsed parsed;
    unsigned char *zero = (unsigned char *)&parsed;
    for (unsigned i = 0; i < sizeof(parsed); ++i) zero[i] = 0;
    unsigned nav = kind == U5_NAV_UPDATE || kind == U5_NAV_MANEUVER || kind == U5_NAV_LANES;
    if (enabled && p && n && (nav || kind == U5_RX_REJECTED || kind == U5_GPS_REQUEST))
        u5hud_parse_frame(p, n, mid, &parsed);
    if (kind == U5_FRAME_PENDING) parsed.result = U5_INCOMPLETE;
    if (kind == U5_FRAME_ERROR) parsed.result = reason == U5_FRAME_LIMIT ? U5_OVERSIZE : U5_BAD_HEADER;
    if (!u5hud_try_lock(shared, s->pid)) {
        if (enabled && kind) __atomic_add_fetch(&shared->trace_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    u5hud_write_begin(shared);
    if (!s->trace_session || s->new_session || shared->session_pid != s->pid) {
        s->trace_session = ++shared->trace_session;
        s->new_session = 0;
        if (enabled) u5hud_new_event(shared, U5_SESSION_BEGIN, now[0], now[1] / 1000000);
    }
    if (changed || kind == U5_IDENTIFY || kind == U5_ACCEPTED) {
        shared->session_pid = s->pid;
        shared->session_accepted = s->accepted;
        shared->session_accepted_seconds = s->accepted ? now[0] : 0;
    }
    shared->protocol_ready = s->protocol_ready;
    shared->native_pass_known = s->native_pass_known;
    shared->native_pass_enabled = s->native_pass_enabled;
    shared->accepted_dispatch_complete = s->accepted_dispatch_complete;
    shared->callback_known = s->callback_known;
    shared->callback_present = s->callback_present;
    shared->subscription_trigger = s->subscription_trigger;
    shared->subscription_attempted = s->subscribe;
    shared->rx_pending_bytes = s->rx_used;
    shared->rx_expected_bytes = s->rx_used >= 6 ? be16(s->rx_frame + 2) : 0;
    if (enabled) {
        struct U5HudDiagnostics *d = &shared->diagnostics;
        d->identify = s->identify; d->accepted = s->accepted;
        d->subscribe = s->subscribe; d->subscribe_result = s->subscribe_result;
        switch (kind) {
        case U5_RX_REJECTED: ++d->rejected; break;
        case U5_NAV_UPDATE: ++d->rx_5201; break;
        case U5_NAV_MANEUVER: ++d->rx_5202; break;
        case U5_NAV_LANES: ++d->rx_5204; break;
        case U5_GPS_REQUEST: ++d->gps_requests; break;
        case U5_GPS_STOP: ++d->gps_stops; break;
        case U5_GPS_RECEIVED:
            ++d->gps_received; d->gps_last_seconds = now[0]; d->gps_fix = auxiliary;
            if (s->gps_trace_seconds && now[0] - s->gps_trace_seconds < 5 && s->gps_trace_fix == auxiliary) kind = 0;
            else { s->gps_trace_seconds = now[0]; s->gps_trace_fix = auxiliary; }
            break;
        }
        if (nav) {
            if (parsed.result == U5_PARSE_OK || parsed.result == U5_SHAPE_WARNING) ++shared->nav_parse_ok;
            else ++shared->nav_parse_bad;
            shared->nav_unknown += parsed.unknown_count;
            shared->nav_shape_warnings += parsed.warnings;
            shared->nav_last_seconds = now[0]; shared->nav_last_message = mid;
            flags |= U5_REFERENCE_SCHEMA;
        }
        if (kind) {
            unsigned captured = n < U5HUD_FRAME_BYTES ? n : U5HUD_FRAME_BYTES;
            unsigned group = 0;
            for (unsigned offset = 0;;) {
                struct U5HudEvent *e = u5hud_new_event(shared, offset ? U5_RAW_CONTINUATION : kind,
                                                      now[0], now[1] / 1000000);
                unsigned bytes = captured - offset;
                if (bytes > U5HUD_RAW_BYTES) bytes = U5HUD_RAW_BYTES;
                if (!offset) group = e->sequence;
                e->group = group; e->message_id = mid; e->wire_length = n;
                if (p && n >= 6 && !(flags & U5_RAW_SELECTED_TLV) && be16(p) == 0x4040)
                    e->wire_length = be16(p + 2);
                e->raw_length = bytes; e->raw_offset = offset;
                e->result = parsed.result; e->reason = reason ? reason : parsed.reason;
                e->error_offset = parsed.error_offset; e->tlv_count = parsed.tlv_count;
                e->known_mask = parsed.known_mask; e->unknown_count = parsed.unknown_count;
                e->flags = flags | (offset ? U5_RAW_CONTINUED : 0) |
                           (captured > U5HUD_RAW_BYTES ? U5_RAW_MORE : 0) |
                           (captured < n ? U5_RAW_TRUNCATED : 0);
                e->auxiliary = auxiliary;
                for (unsigned i = 0; i < 16; ++i) e->values[i] = parsed.values[i];
                if (kind == U5_IDENTIFY) {
                    e->values[0] = s->identify; e->values[1] = s->identify_original;
                    e->values[2] = s->identify_used; e->values[3] = s->identify_capacity;
                }
                if (kind == U5_ACCEPTED || kind == U5_ACCEPTED_DISPATCH_DONE || kind == U5_NATIVE_PASS_APPLIED ||
                    kind == U5_PROTOCOL_READY || kind == U5_TRANSPORT_TX ||
                    kind == U5_SUBSCRIBE_ATTEMPT || kind == U5_SUBSCRIBE_RESULT)
                    diagnostic_transport_state(s, e);
                for (unsigned i = 0; i < sizeof(e->text); ++i) e->text[i] = parsed.text[i];
                for (unsigned i = 0; i < bytes; ++i) e->raw[i] = p[offset + i];
                offset += bytes;
                if (offset >= captured) break;
            }
            if (captured < n || (flags & U5_RAW_TRUNCATED)) ++shared->trace_truncated;
        }
    }
    u5hud_unlock(shared);
}

static void diagnostic_traffic(struct ProbeState *s, unsigned direction,
                               const unsigned char *p, unsigned n, unsigned channel) {
    struct U5HudShared *shared = diagnostic_mapping(s);
    if (!shared || !__atomic_load_n(&shared->enabled, __ATOMIC_ACQUIRE)) return;
    int now[2] = {0, 0};
    linux_call(263, 1, (long)now, 0, 0, 0, 0);
    unsigned framed = n >= 6 && p && be16(p) == 0x4040 && be16(p + 2) >= 6;
    unsigned mid = framed ? be16(p + 4) : 0x10000;
    if (!u5hud_try_lock(shared, s->pid)) {
        __atomic_add_fetch(&shared->trace_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    u5hud_write_begin(shared);
    ++shared->traffic_chunks[direction];
    shared->traffic_bytes[direction] += n;
    shared->traffic_last_seconds[direction] = now[0];
    shared->traffic_last_message[direction] = mid;
    struct U5HudMessageCount *entry = 0;
    unsigned first = 0;
    for (unsigned i = 0; i < U5HUD_MESSAGE_SLOTS; ++i) {
        struct U5HudMessageCount *candidate = &shared->messages[direction][i];
        if (!candidate->chunks || (candidate->message_id == mid && candidate->channel == channel)) {
            entry = candidate; first = !entry->chunks; break;
        }
    }
    if (entry) {
        entry->message_id = mid; entry->channel = channel;
        ++entry->chunks; entry->bytes += n; entry->last_seconds = now[0];
    } else ++shared->traffic_unlisted[direction];
    u5hud_unlock(shared);
    unsigned nav_tx = direction == 1 && framed && (mid == 0x5200 || mid == 0x5203);
    if (first || nav_tx) {
        const unsigned kinds[] = {U5_LINK_RX, U5_TRANSPORT_TX, U5_HU_TX};
        /* Only route subscription/stop payloads are copied. Generic traffic
         * records expose message IDs and counts, never auth/media contents. */
        diagnostic_emit(s, kinds[direction], mid, nav_tx ? p : 0, nav_tx ? n : 0,
                        0, channel, 0);
    }
}

static void diagnostic_event(struct ProbeState *s, unsigned event, unsigned fix) {
    diagnostic_emit(s, event == DIAG_GPS_RECEIVED ? U5_GPS_RECEIVED : 0,
                    event == DIAG_GPS_RECEIVED ? 0xfffb : 0, 0, 0, 0, fix, 0);
}

static unsigned diagnostic_kind(unsigned mid) {
    switch (mid) {
    case 0x1d00: return U5_RX_START; case 0x1d02: return U5_RX_ACCEPTED;
    case 0x1d03: return U5_RX_REJECTED; case 0x1d05: return U5_RX_CANCEL;
    case 0x5201: return U5_NAV_UPDATE; case 0x5202: return U5_NAV_MANEUVER;
    case 0x5204: return U5_NAV_LANES; case 0xfffa: return U5_GPS_REQUEST;
    case 0xfffc: return U5_GPS_STOP; default: return 0;
    }
}

/* R9 reassembles navigation independently of optional logging. Native routing
 * stays unchanged, and no Bluetooth operation runs in this receive hook. */
static void diagnostic_observe(struct ProbeState *s, const unsigned char *p, unsigned n) {
    struct U5HudShared *shared = diagnostic_mapping(s);
    if (!shared) {
        s->rx_used = s->rx_skip = s->rx_reported = 0; return;
    }
    unsigned generation = __atomic_load_n(&shared->generation, __ATOMIC_ACQUIRE);
    if (s->rx_generation != generation) s->rx_reported = 0;
    s->rx_generation = generation;
    while (n) {
        if (s->rx_skip) {
            unsigned skip = s->rx_skip < n ? s->rx_skip : n;
            s->rx_skip -= skip; p += skip; n -= skip; continue;
        }
        unsigned target = s->rx_used < 6 ? 6 : be16(s->rx_frame + 2);
        unsigned take = target - s->rx_used;
        if (take > n) take = n;
        for (unsigned i = 0; i < take; ++i) s->rx_frame[s->rx_used++] = *p++;
        n -= take;
        if (s->rx_used < 6) continue;
        unsigned length = be16(s->rx_frame + 2), mid = be16(s->rx_frame + 4);
        if (be16(s->rx_frame) != 0x4040 || length < 6) {
            nav_discontinuity(s,0);
            diagnostic_emit(s, U5_FRAME_ERROR, mid, s->rx_frame, s->rx_used, U5_INVALID_LENGTH, 0, 0);
            s->rx_used = s->rx_reported = 0; return;
        }
        if (length > sizeof(s->rx_frame)) {
            if(mid==0x5201||mid==0x5202||mid==0x5204)nav_discontinuity(s,1);
            unsigned more = length - 6;
            if (more > U5HUD_RAW_BYTES - 6) more = U5HUD_RAW_BYTES - 6;
            if (more > n) more = n;
            for (unsigned i = 0; i < more; ++i) s->rx_frame[s->rx_used++] = *p++;
            n -= more;
            s->rx_skip = length - s->rx_used;
            if (diagnostic_kind(mid))
                diagnostic_emit(s, U5_FRAME_ERROR, mid, s->rx_frame, s->rx_used, U5_FRAME_LIMIT, length, U5_RAW_TRUNCATED);
            s->rx_used = s->rx_reported = 0; continue;
        }
        if (s->rx_used < length) {
            if (!n && diagnostic_kind(mid)) {
                if (!s->rx_reported) {
                    s->rx_reported = 1;
                    diagnostic_emit(s, U5_FRAME_PENDING, mid, s->rx_frame, s->rx_used, U5_NEED_MORE, length, 0);
                } else diagnostic_event(s, DIAG_STATE, 0);
            }
            continue;
        }
        s->rx_used = s->rx_reported = 0;
        unsigned kind = diagnostic_kind(mid);
        if(mid==0x5201||mid==0x5202||mid==0x5204)nav_publish(s,s->rx_frame,length);
        if(mid==0x1d00||mid==0x1d03||mid==0x1d05)nav_lifecycle(s,0,0);
        if (kind) diagnostic_emit(s, kind, mid, s->rx_frame, length, 0, 0, 0);
    }
    if (shared->rx_pending_bytes != s->rx_used) diagnostic_event(s, DIAG_STATE, 0);
}
