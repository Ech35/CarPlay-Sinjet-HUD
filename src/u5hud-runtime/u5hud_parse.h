#ifndef U5HUD_PARSE_H
#define U5HUD_PARSE_H
#include "u5hud_shared.h"

/* Semantic names/types are a local RGI reference map, not a claim that an
 * unknown iOS field is invalid. Structural checks and raw capture are separate. */
enum U5HudFieldType { U5_BLOB, U5_U8, U5_U16, U5_U32, U5_U64, U5_TEXT,
    U5_U16_LIST, U5_I16_LIST, U5_I16, U5_FLAG, U5_LANE_GROUP };

static inline unsigned u5hud_be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static inline unsigned u5hud_be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}
static inline unsigned u5hud_field_type(unsigned mid, unsigned id) {
    if (mid == 0x1d03) return id == 6 || id == 7 ? U5_U16_LIST :
        id <= 19 || id == 30 ? U5_FLAG : U5_BLOB;
    if (mid == 0xfffa) return id == 0 ? U5_U32 : id <= 3 ? U5_FLAG : U5_BLOB;
    if (mid == 0x5200) return id == 0 ? U5_U16 : id <= 3 ? U5_FLAG : U5_BLOB;
    if (mid == 0x5201) {
        switch (id) {
        case 0: case 13: return U5_U16_LIST;
        case 1: case 2: case 9: case 12: case 15: case 18: return U5_U8;
        case 3: case 4: case 8: case 11: case 19: return U5_TEXT;
        case 5: case 6: return U5_U64;
        case 7: case 10: return U5_U32;
        case 14: case 16: case 17: return U5_U16;
        case 20: return U5_U8;
        }
    } else if (mid == 0x5202) {
        switch (id) {
        case 0: return U5_U16_LIST;
        case 1: case 12: return U5_U16;
        case 2: case 4: case 6: case 13: return U5_TEXT;
        case 3: case 7: case 8: case 9: return U5_U8;
        case 5: return U5_U32;
        case 10: return U5_I16_LIST;
        case 11: return U5_I16;
        }
    } else if (mid == 0x5204) {
        switch (id) {
        case 0: return U5_U16_LIST;
        case 1: return U5_U16;
        case 2: return U5_LANE_GROUP;
        case 3: return U5_TEXT;
        }
    }
    return U5_BLOB;
}

struct U5HudParsed {
    unsigned result, reason, error_offset, tlv_count, known_mask, unknown_count, warnings;
    unsigned values[16];
    unsigned char text[96];
};

static inline int u5hud_sequence_valid(const unsigned char *p, unsigned n) {
    for (unsigned off = 0; off < n;) {
        if (n - off < 4) return 0;
        unsigned len = u5hud_be16(p + off);
        if (len < 4 || len > n - off) return 0;
        off += len;
    }
    return 1;
}

static inline int u5hud_lane_check(const unsigned char *p, unsigned n, unsigned base,
                                 unsigned depth, struct U5HudParsed *out) {
    for (unsigned off = 0; off < n;) {
        if (n - off < 4) { out->error_offset = base + off; return 0; }
        unsigned len = u5hud_be16(p + off), id = u5hud_be16(p + off + 2);
        if (len < 4 || len > n - off) { out->error_offset = base + off; return 0; }
        unsigned bytes = len - 4;
        const unsigned char *value = p + off + 4;
        ++out->tlv_count;
        if (id == 0 && bytes > 2 && depth < 3 && u5hud_sequence_valid(value, bytes)) {
            if (!u5hud_lane_check(value, bytes, base + off + 4, depth + 1, out)) return 0;
        } else if (id == 0) {
            if (bytes == 2) ++out->values[15];
            else ++out->warnings;
        } else if (id == 1) {
            if (bytes != 1) ++out->warnings;
        } else if (id == 2 || id == 3) {
            if (!bytes || (bytes & 1)) ++out->warnings;
        } else ++out->unknown_count;
        off += len;
    }
    return 1;
}

static inline void u5hud_parse_frame(const unsigned char *p, unsigned n, unsigned mid,
                                    struct U5HudParsed *out) {
    unsigned char *zero = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof(*out); ++i) zero[i] = 0;
    out->result = U5_PARSE_OK;
    if (n < 6 || u5hud_be16(p) != 0x4040 || u5hud_be16(p + 2) != n) {
        out->result = U5_BAD_HEADER;
        out->reason = U5_INVALID_LENGTH;
        return;
    }
    for (unsigned off = 6; off < n;) {
        if (n - off < 4) {
            out->result = U5_BAD_TLV; out->reason = U5_SHORT_TLV;
            out->error_offset = off; return;
        }
        unsigned len = u5hud_be16(p + off), id = u5hud_be16(p + off + 2);
        if (len < 4 || len > n - off) {
            out->result = U5_BAD_TLV; out->reason = U5_BAD_TLV_LENGTH;
            out->error_offset = off; return;
        }
        unsigned bytes = len - 4, type = u5hud_field_type(mid, id);
        const unsigned char *value = p + off + 4;
        ++out->tlv_count;
        if (type == U5_BLOB) ++out->unknown_count;
        else if (id < 32) out->known_mask |= 1u << id;
        unsigned expected = type == U5_U8 ? 1 : type == U5_U16 || type == U5_I16 ? 2 :
                            type == U5_U32 ? 4 : type == U5_U64 ? 8 : 0;
        if (expected && bytes != expected) ++out->warnings;
        if ((type == U5_U16_LIST || type == U5_I16_LIST) && (bytes & 1)) ++out->warnings;
        if (type == U5_FLAG && bytes) ++out->warnings;
        if (type == U5_TEXT) {
            if (bytes && value[bytes - 1]) ++out->warnings;
            if (!out->text[0]) {
                unsigned count = bytes < sizeof(out->text) - 1 ? bytes : sizeof(out->text) - 1;
                for (unsigned i = 0; i < count; ++i) out->text[i] = value[i];
            }
        }
        if (id < 16 && bytes >= expected && expected && expected <= 4)
            out->values[id] = expected == 1 ? value[0] : expected == 2 ? u5hud_be16(value) : u5hud_be32(value);
        if (type == U5_U16_LIST && id < 16 && bytes >= 2) out->values[id] = u5hud_be16(value);
        if (type == U5_LANE_GROUP && !u5hud_lane_check(value, bytes, off + 4, 0, out)) {
            out->result = U5_BAD_TLV; out->reason = U5_BAD_TLV_LENGTH; return;
        }
        off += len;
    }
    if (out->warnings) { out->result = U5_SHAPE_WARNING; out->reason = U5_VALUE_SHAPE; }
    if (mid == 0x1d03 && !out->tlv_count) out->reason = U5_REJECT_NO_DETAILS;
}
#endif
