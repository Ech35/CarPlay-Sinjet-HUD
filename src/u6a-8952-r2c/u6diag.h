#ifndef U6_DIAG_H
#define U6_DIAG_H
#include <stdint.h>

#define DIAG_MAGIC 0x55365232u
#define DIAG_DIR "/tmp/u6diag"
#define DIAG_PATH DIAG_DIR "/state-r2"
#define DIAG_BUILD "ly8952-26052617-u6a-hud-r2c"
#define DIAG_LOG_FILENAME "u6a-8952-r2c-log.txt"
#define DIAG_CAPACITY 1024u
#define DIAG_RAW_BYTES 384u
#define COMPONENT_ID 0x7f01u
#define CARPLAY_MARKER "/tmp/lywcpok"

enum Phase { WAITING, IDENTIFYING, IDENT_ACCEPTED, SUBMITTED, RECEIVING, REJECTED, STOCK_ONLY, SEND_FAILED };
enum Event {
    IDENT_START, IDENT_SENT, IDENT_RESULT, IDENT_REJECT, LINK_READY, SUBMIT_TRY, SUBMIT_RESULT,
    NAV_RX, NAV_INVALID, LINK_END, OBSERVED_OTHER_MODE, AUTH_SUCCESS, MARKER_OBSERVED,
    IDENT_COMPONENT_WIRE, IDENT_TX_WIRE, IDENT_RX_WIRE, SUBMIT_HANDOFF, CP_SEND_ENTER, CP_SEND_RETURN,
    CP_SEND_SKIPPED, NAV_INCOMPLETE, NAV_OVERSIZED, AUTH_COMPLETE
};
enum IdentifyResult {
    ADDED, ALREADY_PRESENT, NO_LISTS, INVALID_LAYOUT, NO_SPACE, COMPONENT_CONFLICT, STOCK_RETRY
};

struct EventRecord {
    uint32_t stamp, session, time_ms, type, message, frame_bytes, raw_bytes, detail, extra;
    unsigned char raw[DIAG_RAW_BYTES];
};
struct Diag {
    uint32_t magic, enabled, head, cleared, dropped, session, phase;
    uint32_t ident_sent, accepted, rejected, subscriptions, nav_route, nav_turn, nav_lane, nav_invalid;
    uint32_t types_seen, callback_ready, pass_mode, last_result;
    uint32_t producer_pid, control_channel;
    uint32_t auth_success, marker_seen, submit_return, control_tx;
    uint32_t cp_calls, cp_returns, cp_return, cp_skipped;
    uint32_t rx_control_chunks, rx_other_chunks, rx_fragmented, rx_combined, rx_oversized, route_7f01;
    uint32_t auth_complete_session;
    struct EventRecord records[DIAG_CAPACITY];
};
#endif
