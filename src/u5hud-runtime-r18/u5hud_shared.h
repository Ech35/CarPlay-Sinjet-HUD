#ifndef U5HUD_SHARED_H
#define U5HUD_SHARED_H

#include <stdint.h>

#define U5HUD_MAGIC 0x55354836u
#define U5HUD_PAGE_SIZE 4096
#define U5HUD_STATE_BYTES (1024u * 1024u)
#define U5HUD_EVENT_CAPACITY 1024u
#define U5HUD_RAW_BYTES 768u
#define U5HUD_FRAME_BYTES 8192u
#define U5HUD_RAM_DIR "/tmp/u5hud"
#define U5HUD_STATE_PATH U5HUD_RAM_DIR "/state-v6"
#define U5HUD_CONFIG_PATH "/mnt/UDISK/u5hud-log.enabled"
#define U5HUD_BUILD "ly2734-26051317-u5hud-r18"
#define U5HUD_MESSAGE_SLOTS 24u

enum U5HudEventKind {
    U5_INIT = 1, U5_LOG_ENABLED, U5_LOG_DISABLED, U5_CLEAR,
    U5_SESSION_BEGIN, U5_IDENTIFY, U5_TX_LIST, U5_RX_LIST,
    U5_ACCEPTED, U5_SUBSCRIBE_ATTEMPT, U5_SUBSCRIBE_RESULT,
    U5_RX_START, U5_RX_ACCEPTED, U5_RX_REJECTED, U5_RX_CANCEL,
    U5_NAV_UPDATE, U5_NAV_MANEUVER, U5_NAV_LANES,
    U5_GPS_REQUEST, U5_GPS_STOP, U5_GPS_RECEIVED,
    U5_FRAME_PENDING, U5_FRAME_ERROR, U5_RAW_CONTINUATION,
    U5_PROTOCOL_READY, U5_TRANSPORT_TX, U5_LINK_RX, U5_HU_TX,
    U5_OTHER_CHANNEL_NAV, U5_NATIVE_PASS_STATE,
    U5_ACCEPTED_DISPATCH_DONE, U5_NATIVE_PASS_APPLIED
};
enum U5HudParseResult { U5_NOT_PARSED, U5_PARSE_OK, U5_BAD_HEADER, U5_INCOMPLETE,
    U5_BAD_TLV, U5_SHAPE_WARNING, U5_OVERSIZE };
enum U5HudReason { U5_REASON_NONE, U5_INVALID_LENGTH, U5_SHORT_TLV, U5_BAD_TLV_LENGTH,
    U5_NO_CAPACITY, U5_COMPONENT_COLLISION, U5_MISSING_LIST, U5_DUPLICATE_LIST,
    U5_REJECT_FALLBACK, U5_STOCK_COMPONENT, U5_COMPONENT_ADDED,
    U5_VALUE_SHAPE, U5_FRAME_LIMIT, U5_REJECT_NO_DETAILS, U5_NEED_MORE };
enum { U5_RAW_MORE = 1, U5_RAW_TRUNCATED = 2, U5_RAW_SELECTED_TLV = 4,
       U5_REFERENCE_SCHEMA = 8, U5_RAW_CONTINUED = 16 };

struct U5HudEvent {
    uint32_t sequence, seconds, milliseconds, session;
    uint32_t kind, message_id, wire_length, raw_length;
    uint32_t result, reason, error_offset, tlv_count;
    uint32_t known_mask, unknown_count, flags, group;
    uint32_t raw_offset, auxiliary;
    uint32_t values[16];
    unsigned char text[96];
    unsigned char raw[U5HUD_RAW_BYTES];
};

struct U5HudDiagnostics {
    uint32_t identify;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t subscribe;
    int32_t subscribe_result;
    uint32_t rx_5201;
    uint32_t rx_5202;
    uint32_t rx_5204;
    uint32_t gps_requests;
    uint32_t gps_stops;
    uint32_t gps_received;
    uint32_t gps_last_seconds;
    uint32_t gps_fix;
};

/* Only message headers are counted here. Authentication and media payloads
 * are never copied into the message census. Direction: 0=phone RX,
 * 1=accessory transport TX, 2=HU-to-phone entry. One observation is a chunk,
 * so a split/coalesced chunk is not claimed to be one complete frame. */
struct U5HudMessageCount {
    uint32_t message_id, channel, chunks, bytes, last_seconds;
};

struct U5HudShared {
    uint32_t magic;
    uint32_t enabled;
    uint32_t owner;
    uint32_t sequence;
    uint32_t generation;
    uint32_t cleared_seconds;
    struct U5HudDiagnostics diagnostics;
    /* Connection state remains active when optional diagnostic logging is off. */
    uint32_t session_pid;
    uint32_t session_accepted;
    uint32_t session_accepted_seconds;
    uint32_t trace_next, trace_count, trace_overwritten, trace_dropped;
    uint32_t trace_session, trace_truncated;
    uint32_t nav_parse_ok, nav_parse_bad, nav_unknown, nav_shape_warnings;
    uint32_t nav_last_seconds, nav_last_message;
    uint32_t rx_pending_bytes, rx_expected_bytes;
    uint32_t protocol_ready;
    uint32_t traffic_chunks[3], traffic_bytes[3], traffic_last_seconds[3];
    uint32_t traffic_last_message[3], traffic_unlisted[3];
    struct U5HudMessageCount messages[3][U5HUD_MESSAGE_SLOTS];
    uint32_t native_pass_known, native_pass_enabled;
    /* Append only: existing v6 header fields and event-ring ABI stay fixed. */
    uint32_t accepted_dispatch_complete, callback_known, callback_present;
    uint32_t subscription_trigger, subscription_attempted;
};

_Static_assert(sizeof(struct U5HudShared) <= U5HUD_PAGE_SIZE, "header exceeds one page");
_Static_assert(sizeof(struct U5HudEvent) == 1000, "trace record ABI changed");
_Static_assert(U5HUD_PAGE_SIZE + U5HUD_EVENT_CAPACITY * sizeof(struct U5HudEvent) <= U5HUD_STATE_BYTES,
               "trace exceeds fixed RAM budget");

static inline struct U5HudEvent *u5hud_event_at(void *shared, unsigned sequence) {
    return (struct U5HudEvent *)((unsigned char *)shared + U5HUD_PAGE_SIZE) +
           ((sequence - 1) % U5HUD_EVENT_CAPACITY);
}

/* Caller owns the shared writer lock. Raw bytes outside raw_length are ignored. */
static inline struct U5HudEvent *u5hud_new_event(struct U5HudShared *shared, unsigned kind,
                                               unsigned seconds, unsigned milliseconds) {
    unsigned sequence = ++shared->trace_next;
    struct U5HudEvent *event = u5hud_event_at(shared, sequence);
    unsigned char *bytes = (unsigned char *)event;
    for (unsigned i = 0; i < sizeof(*event) - U5HUD_RAW_BYTES; ++i) bytes[i] = 0;
    event->sequence = event->group = sequence;
    event->kind = kind;
    event->seconds = seconds;
    event->milliseconds = milliseconds;
    event->session = shared->trace_session;
    if (shared->trace_count < U5HUD_EVENT_CAPACITY) ++shared->trace_count;
    else ++shared->trace_overwritten;
    return event;
}

static inline int u5hud_try_lock(struct U5HudShared *shared, unsigned pid) {
    unsigned expected = 0;
    return __atomic_compare_exchange_n(&shared->owner, &expected, pid, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void u5hud_write_begin(struct U5HudShared *shared) {
    __atomic_add_fetch(&shared->sequence, 1, __ATOMIC_ACQ_REL);
}

static inline void u5hud_unlock(struct U5HudShared *shared) {
    __atomic_add_fetch(&shared->sequence, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&shared->owner, 0, __ATOMIC_RELEASE);
}

#endif
