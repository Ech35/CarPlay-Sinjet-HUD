#include <stddef.h>
#include <stdint.h>
#include "../u5hud-runtime/u5hud_shared.h"

#define PROBE_ID 0x7f01
#define PROBE_STATE 0x2e490

enum { DECLINED, STOCK_COMPONENT, PROBE_ADDED };
struct ProbeState {
    void *ctx;
    unsigned identify, accepted, subscribe;
    int subscribe_result;
    unsigned reject;
    struct U5HudShared *shared;
    unsigned pid;
    unsigned hu_used, hu_generation;
    unsigned char hu_frame[4096];
    unsigned trace_session, new_session, identify_reason;
    unsigned identify_used, identify_capacity, identify_original;
    unsigned gps_trace_seconds, gps_trace_fix;
    unsigned rx_used, rx_generation, rx_skip, rx_reported;
    unsigned char rx_frame[U5HUD_FRAME_BYTES];
    unsigned protocol_ready;
    unsigned native_pass_known, native_pass_enabled;
    unsigned accepted_dispatch_complete, callback_known, callback_present, subscription_trigger;
    struct NavRing *nav;
};
_Static_assert(sizeof(struct ProbeState) <= 0x4000, "probe state exceeds reserved BSS");

static unsigned be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static void put16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void *ptr(const void *p, unsigned off) {
    return *(void *const *)((const char *)p + off);
}
static struct ProbeState *state(uintptr_t base) {
    return (struct ProbeState *)(base + PROBE_STATE);
}
#include "diagnostics.h"
static unsigned contains(const unsigned char *p, unsigned n, unsigned id) {
    for (unsigned i = 0; i < n; i += 2)
        if (be16(p + i) == id) return 1;
    return 0;
}
static const uint16_t tx_ids[] = {0x5200, 0x5203};
static const uint16_t rx_ids[] = {0x5201, 0x5202, 0x5204};
static const unsigned char component_name[] = "RouteGuidanceDisplayComponent";
#define COMPONENT_SIZE (4 + 8 * 6 + 4 + sizeof(component_name))

static int decline(unsigned *reason, unsigned value) { *reason = value; return DECLINED; }
static int insert_component(unsigned char *p, unsigned *used, unsigned cap, unsigned *reason) {
    unsigned n = *used, list[2] = {0, 0};
    unsigned add[2] = {0, 0}, exists = 0;
    if (n < 6 || n > cap || n > 65535) return decline(reason, U5_INVALID_LENGTH);
    for (unsigned off = 6; off < n;) {
        if (n - off < 4) return decline(reason, U5_SHORT_TLV);
        unsigned len = be16(p + off), id = be16(p + off + 2);
        if (len < 4 || len > n - off) return decline(reason, U5_BAD_TLV_LENGTH);
        if (id == 0x001e) exists = 1;
        // A nested ID=0 with this value may already identify another component.
        if (id >= 10 && len >= 10 && be16(p + off + 4) == 6 &&
            be16(p + off + 6) == 0 && be16(p + off + 8) == PROBE_ID)
            return decline(reason, U5_COMPONENT_COLLISION);
        if (id == 6 || id == 7) {
            unsigned k = id - 6;
            if (list[k]) return decline(reason, U5_DUPLICATE_LIST);
            if (len & 1) return decline(reason, U5_VALUE_SHAPE);
            list[k] = off;
            const uint16_t *ids = k ? rx_ids : tx_ids;
            for (unsigned i = 0; i < k + 2; ++i)
                if (!contains(p + off + 4, len - 4, ids[i])) add[k] += 2;
        }
        off += len;
    }
    if (exists) { *reason = U5_STOCK_COMPONENT; return STOCK_COMPONENT; }
    if (!list[0] || !list[1]) return decline(reason, U5_MISSING_LIST);
    unsigned extra = add[0] + add[1] + COMPONENT_SIZE;
    if (extra > cap - n || extra > 65535 - n) return decline(reason, U5_NO_CAPACITY);
    // Rewrite the later TLV first so the earlier TLV's original offset stays valid.
    unsigned k = list[0] > list[1] ? 0 : 1;
    for (unsigned pass = 0; pass < 2; ++pass, k ^= 1) {
        if (!add[k]) continue;
        unsigned off = list[k], len = be16(p + off), end = off + len;
        for (unsigned i = n; i > end; --i) p[i - 1 + add[k]] = p[i - 1];
        const uint16_t *ids = k ? rx_ids : tx_ids;
        for (unsigned i = 0; i < k + 2; ++i) {
            if (!contains(p + off + 4, len - 4, ids[i])) {
                put16(p + end, ids[i]);
                end += 2;
            }
        }
        put16(p + off, len + add[k]);
        n += add[k];
    }
    put16(p + n, COMPONENT_SIZE);
    put16(p + n + 2, 0x001e);
    unsigned at = n + 4;
    const unsigned values[] = {PROBE_ID, 0, 256, 256, 256, 256, 6, 256, 6};
    for (unsigned id = 0; id < 9; ++id) {
        if (id == 1) {
            put16(p + at, 4 + sizeof(component_name));
            put16(p + at + 2, id);
            at += 4;
            for (unsigned i = 0; i < sizeof(component_name); ++i) p[at++] = component_name[i];
        } else {
            put16(p + at, 6);
            put16(p + at + 2, id);
            put16(p + at + 4, values[id]);
            at += 6;
        }
    }
    *used = at;
    *reason = U5_COMPONENT_ADDED;
    return PROBE_ADDED;
}

void identify_probe(void *ctx, unsigned id, void *record, uintptr_t base) {
    if (id != 0x1d01 || (uintptr_t)ptr(ctx, 0x48) != base + 0xcd90) return;
    void *owner = ptr(ctx, 0);
    // The Wi-Fi constructor stores its main control context in owner+0x104.
    if (!owner || ptr(owner, 0x104) != ctx) return;
    struct ProbeState *s = state(base);
    int rejected_probe = s->ctx == ctx && s->reject;
    s->new_session = s->ctx != ctx;
    s->rx_used = s->rx_skip = s->rx_reported = 0;
    s->ctx = ctx;
    s->accepted = s->subscribe = 0;
    s->protocol_ready = 0;
    s->accepted_dispatch_complete = s->callback_known = s->callback_present = s->subscription_trigger = 0;
    s->native_pass_known = s->native_pass_enabled = 0;
    if (!rejected_probe) s->reject = 0;
    s->subscribe_result = 0;
    s->hu_used = 0;
    s->identify_original = ((unsigned *)record)[1];
    s->identify_capacity = ((unsigned *)record)[2];
    s->identify_reason = rejected_probe ? U5_REJECT_FALLBACK : 0;
    s->identify = rejected_probe ? DECLINED :
        insert_component(ptr(record, 12), (unsigned *)record + 1, s->identify_capacity, &s->identify_reason);
    s->identify_used = ((unsigned *)record)[1];
    nav_lifecycle(s,1,0);
    const unsigned char *packet = ptr(record, 12), *component = 0;
    unsigned component_bytes = 0;
    for (unsigned off = 6; off < s->identify_used && s->identify_used <= s->identify_capacity;) {
        if (s->identify_used - off < 4) break;
        unsigned len = be16(packet + off);
        if (len < 4 || len > s->identify_used - off) break;
        if (be16(packet + off + 2) == 30) { component = packet + off; component_bytes = len; }
        off += len;
    }
    diagnostic_emit(s, U5_IDENTIFY, id, component, component_bytes, s->identify_reason,
                    s->identify_used, U5_RAW_SELECTED_TLV);
    for (unsigned off = 6; off < s->identify_used && s->identify_used <= s->identify_capacity;) {
        if (s->identify_used - off < 4) break;
        unsigned len = be16(packet + off), tag = be16(packet + off + 2);
        if (len < 4 || len > s->identify_used - off) break;
        if (tag == 6 || tag == 7)
            diagnostic_emit(s, tag == 6 ? U5_TX_LIST : U5_RX_LIST, id, packet + off, len, 0, 0, U5_RAW_SELECTED_TLV);
        off += len;
    }
}

static void make_subscription(void *ctx, void *opaque) {
    uintptr_t base = (uintptr_t)opaque;
    typedef void (*append_fn)(void *, unsigned, const void *, int, void *, void *);
    append_fn append = (append_fn)(base + 0xf920);
    static const unsigned char component[] = {PROBE_ID >> 8, PROBE_ID & 255};
    append(ctx, 0, component, 2, 0, 0);
    append(ctx, 1, 0, 0, 0, 0);
    append(ctx, 2, 0, 0, 0, 0);
    append(ctx, 3, 0, 0, 0, 0);
}

void accepted_probe(void *ctx, void *callbacks, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (s->ctx != ctx || s->accepted || (uintptr_t)ptr(ctx, 0x48) != base + 0xcd90) return;
    s->accepted = 1;
    s->callback_known = 1;
    s->callback_present = callbacks && ptr(callbacks, 0);
    diagnostic_emit(s, U5_ACCEPTED, 0x1d02, 0, 0, 0, 0, 0);
    diagnostic_event(s, DIAG_STATE, 0);
}

/* The success callback is optional. The Wi-Fi path may instead enable native
 * pass-through later through SetiAP2OverWifiCPPassState. Require the accepted
 * dispatch to finish AND the actual owner state to be enabled in either path. */
static void maybe_subscribe(struct ProbeState *s, uintptr_t base, unsigned trigger) {
    if (!s->ctx || !s->accepted || !s->accepted_dispatch_complete ||
        s->identify != PROBE_ADDED || (uintptr_t)ptr(s->ctx, 0x48) != base + 0xcd90) return;
    void *owner = ptr(s->ctx, 0);
    if (!owner || !*(unsigned *)((char *)owner + 0x144)) return;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&s->subscribe, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    typedef int (*send_fn)(void *, unsigned, void (*)(void *, void *), void *, unsigned);
    s->subscription_trigger = trigger;
    diagnostic_emit(s, U5_SUBSCRIBE_ATTEMPT, 0x5200, 0, 0, 0, PROBE_ID, 0);
    s->subscribe_result = ((send_fn)(base + 0x13e40))(
        s->ctx, 0x5200, make_subscription, (void *)base, 0);
    nav_lifecycle(s,0,s->subscribe_result==0);
    diagnostic_emit(s, U5_SUBSCRIBE_RESULT, 0x5200, 0, 0, 0, (unsigned)s->subscribe_result, 0);
}

void ready_probe(void *ctx, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (s->ctx != ctx || !s->accepted || s->protocol_ready ||
        (uintptr_t)ptr(ctx, 0x48) != base + 0xcd90) return;
    s->protocol_ready = 1;
    s->accepted_dispatch_complete = 1;
    diagnostic_emit(s, U5_PROTOCOL_READY, 0x1d02, 0, 0, 0, 0, 0);
    maybe_subscribe(s, base, 1);
    diagnostic_event(s, DIAG_STATE, 0);
}

/* Common native return reached when the optional success callback is null.
 * Other messages share this epilogue, so require this accepted null-callback
 * context and record completion only once. */
void accepted_return_probe(void *ctx, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (s->ctx != ctx || !s->accepted || s->accepted_dispatch_complete ||
        !s->callback_known || s->callback_present ||
        (uintptr_t)ptr(ctx, 0x48) != base + 0xcd90) return;
    s->accepted_dispatch_complete = 1;
    diagnostic_emit(s, U5_ACCEPTED_DISPATCH_DONE, 0x1d02, 0, 0, 0, 0, 0);
    maybe_subscribe(s, base, 2);
    diagnostic_event(s, DIAG_STATE, 0);
}

unsigned received_probe(void *owner, const unsigned char *p, unsigned n,
                        unsigned channel, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (!s->ctx || ptr(owner, 0x104) != s->ctx || !p || !n) return 0;
    diagnostic_traffic(s, 0, p, n, channel);
    /* Observe every iAP session, but preserve the original routing for all
     * channels except the same control-session navigation frames as R5. */
    if (channel != ((const unsigned char *)owner)[0x154]) {
        if (n >= 6 && be16(p) == 0x4040 && be16(p + 2) == n &&
            (be16(p + 4) == 0x5201 || be16(p + 4) == 0x5202 || be16(p + 4) == 0x5204))
            diagnostic_emit(s, U5_OTHER_CHANNEL_NAV, be16(p + 4), p, n, 0, channel, 0);
        return 0;
    }
    diagnostic_observe(s, p, n);
    if (n < 6 || be16(p) != 0x4040) return 0;
    unsigned len = be16(p + 2), id = be16(p + 4);
    if (len < 6 || len > n) return 0;
    if (id == 0x1d00) {
        nav_lifecycle(s,0,0);
        s->ctx = 0;
        s->accepted = s->subscribe = 0;
        s->protocol_ready = 0;
        s->accepted_dispatch_complete = s->callback_known = s->callback_present = s->subscription_trigger = 0;
        diagnostic_event(s, DIAG_STATE, 0);
        return 0;
    }
    if (id == 0x1d03) {
        nav_lifecycle(s,0,0);
        s->reject++;
        diagnostic_event(s, DIAG_STATE, 0);
        return 0;
    }
    if (id == 0xfffa || id == 0xfffc) {
        return 0;
    }
    if (id != 0x5201 && id != 0x5202 && id != 0x5204) return 0;
    diagnostic_event(s, DIAG_STATE, 0);
    // New probe-only messages are observed without changing stock HU guidance.
    return len == n && s->identify == PROBE_ADDED && s->subscribe;
}

/* Entry to the real iAP transport, after the native serializer filled in the
 * header. Observing a call here proves local handoff, not phone receipt. */
void transport_probe(const unsigned char *p, unsigned n, unsigned channel,
                     void *owner, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (!s->ctx || !owner || ptr(owner, 0x104) != s->ctx || !p || !n) return;
    diagnostic_traffic(s, 1, p, n, channel);
}

/* Observe the original lifecycle notification as well as Identify. The stock
 * app calls this with state 0 when its CP iAP session is stopped. */
void pass_state_probe(void *impl, void *callback, unsigned enabled, uintptr_t base) {
    (void)callback;
    struct ProbeState *s = state(base);
    if (!impl || !s->ctx || ptr(impl, 0) != ptr(s->ctx, 0)) return;
    /* A stopped pass-through can resume without a fresh Identify. Preserve
     * subscription bookkeeping; this notification only informs diagnostics. */
    s->native_pass_known = 1;
    s->native_pass_enabled = !!enabled;
    if(!enabled){nav_lifecycle(s,0,0);s->rx_used=s->rx_skip=s->rx_reported=0;}
    diagnostic_emit(s, U5_NATIVE_PASS_STATE, 0, 0, 0, 0, enabled, 0);
}

/* Runs after the native owner+0x144 store, before the original epilogue.
 * It must not initiate a subscription from the earlier notification hook. */
void pass_applied_probe(void *impl, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (!impl || !s->ctx || ptr(impl, 0) != ptr(s->ctx, 0)) return;
    diagnostic_emit(s, U5_NATIVE_PASS_APPLIED, 0, 0, 0, 0, 0, 0);
    maybe_subscribe(s, base, 3);
    if(s->accepted&&s->subscribe&&s->subscribe_result==0)
        nav_lifecycle(s,0,s->native_pass_enabled);
    diagnostic_event(s, DIAG_STATE, 0);
}

void hu_location_probe(void *client, const unsigned char *p, unsigned n, uintptr_t base) {
    struct ProbeState *s = state(base);
    if (!client || !p || !n || !s->ctx) return;
    void *owner = ptr(client, 0);
    if (!owner || ptr(owner, 0x104) != s->ctx) return;
    unsigned channel = p[0] <= 2 ? p[0] : 0;
    diagnostic_traffic(s, 2, p + (p[0] <= 2), n - (p[0] <= 2), channel);
    struct U5HudShared *shared = diagnostic_mapping(s);
    if (!shared || !__atomic_load_n(&shared->enabled, __ATOMIC_ACQUIRE)) {
        s->hu_used = 0;
        return;
    }
    unsigned generation = __atomic_load_n(&shared->generation, __ATOMIC_ACQUIRE);
    if (s->hu_generation != generation) s->hu_used = 0;
    s->hu_generation = generation;
    /* A leading zero identifies the existing control-channel wrapper. */
    if (p[0] <= 2) {
        if (p[0] != 0) return;
        ++p;
        --n;
    }
    while (n) {
        unsigned target = s->hu_used < 6 ? 6 : be16(s->hu_frame + 2);
        unsigned take = target - s->hu_used;
        if (take > n) take = n;
        for (unsigned i = 0; i < take; ++i) s->hu_frame[s->hu_used++] = *p++;
        n -= take;
        if (s->hu_used < 6) continue;
        unsigned length = be16(s->hu_frame + 2);
        if (be16(s->hu_frame) != 0x4040 || length < 6 || length > sizeof(s->hu_frame)) {
            s->hu_used = 0;
            return;
        }
        if (s->hu_used < length) continue;
        if (be16(s->hu_frame + 4) == 0xfffb)
            diagnostic_event(s, DIAG_GPS_RECEIVED, nmea_fix(s->hu_frame + 6, length - 6));
        s->hu_used = 0;
    }
}
