/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HUD_CORE_H
#define HUD_CORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif
typedef struct hud_core hud_core;

/* BASA 221019 / 230927 R2 has six receiver slots. A wider CarPlay lane group
 * is reduced to a contiguous window before any command is sent. */
#define HC_MAX_DISPLAY_LANES 6
enum { HC_IDLE, HC_SCANNING, HC_WAITING, HC_CONNECTING, HC_DISCOVERING, HC_READY };
enum { HC_SCAN=1, HC_STOP_SCAN=2, HC_CONNECT=4, HC_DISCOVER=8, HC_CLOSE=16 };
/* All calls must be serialized by one owner thread. Times are monotonic ms.
 * Actions are requests for an adapter, not evidence of physical radio activity.
 * Generation must be echoed by asynchronous BLE callbacks. */
API hud_core *hc_create(uint16_t component);
API void hc_destroy(hud_core *c);
API unsigned hc_input(hud_core *c,const uint8_t *p,size_t n,uint64_t now);
API unsigned hc_tick(hud_core *c,uint64_t now);
API unsigned hc_found(hud_core *c,uint32_t generation,uint64_t now);
API unsigned hc_connected(hud_core *c,uint32_t generation,uint64_t now);
API unsigned hc_ready(hud_core *c,uint32_t generation,uint64_t now);
API unsigned hc_failed(hud_core *c,uint32_t generation,uint64_t now);
API unsigned hc_disconnected(hud_core *c,uint32_t generation);
API unsigned hc_session_end(hud_core *c);
/* Transport already stopped externally. Keep incremental navigation cache,
 * discard all link/queued-write state. Resume requires NEW valid input. */
API void hc_transport_reset(hud_core *c,int available);
API int hc_phase(const hud_core *c);
API int hc_attempts(const hud_core *c);
API uint32_t hc_generation(const hud_core *c);
/* Returns one complete ATT chunk. Do not request another until hc_written.
 * Caller reports success on write completion, or stack acceptance for NO_RESPONSE.
 * A failed/expired write drops this link; NEW valid data is required to rescan. */
API size_t hc_next_chunk(hud_core *c,uint64_t now,uint8_t *out,size_t mtu_payload);
API unsigned hc_written(hud_core *c,uint32_t generation,int success,uint64_t now);
/* Stack BUSY before accepting a chunk: bounded 16ms retry, no byte consumption. */
API unsigned hc_write_busy(hud_core *c,uint32_t generation,uint64_t now);
API size_t hc_component_tlv(uint8_t *out,size_t capacity,uint16_t component);
API size_t hc_subscription(uint8_t *out,size_t capacity,uint16_t component);
/* Pure packet/table transforms for a future G5 adapter. Not live hooks.
 * Identify requires non-overlapping input/output. 0 = reject, no usable output.
 * Table callbacks are 32-bit ARM addresses, never invoked by this transform. */
API size_t hc_identify(const uint8_t *input,size_t n,uint8_t *out,size_t capacity,uint16_t component);
API int hc_register_receive(uint8_t *table,size_t table_bytes,uint32_t *callbacks,
                           size_t callback_count,const uint32_t handlers[3]);
/* Approved mainland-China mapping. Driving side is intentionally ignored by
 * the compatibility entry point. Explicit unknown types use tunnel icon 16;
 * this does not imply that the vehicle is actually entering a tunnel. */
API int hc_turn_icon(unsigned carplay_type,int driving_side);
/* Optional signed exit angle from 0x5202/0x000B. Presence is independent of
 * value: absent != 0; actual -1 degrees is valid, +/-1000 is not a direction. */
API int hc_turn_icon_angle(unsigned carplay_type,int exit_angle,int angle_present);
API int hc_lane_pair(const int16_t *angles,size_t count,int primary,int status,uint8_t pair[2]);
#endif
