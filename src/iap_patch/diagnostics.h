enum {
    DIAG_STATE, DIAG_REJECT, DIAG_NAV_5201, DIAG_NAV_5202, DIAG_NAV_5204,
    DIAG_GPS_REQUEST, DIAG_GPS_STOP, DIAG_GPS_RECEIVED
};

/* ARM Linux syscalls keep the payload independent of new dynamic imports. */
static long linux_call(unsigned number, long a, long b, long c, long d, long e, long f) {
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3),
                     "r"(r4), "r"(r5), "r"(r7) : "memory", "cc");
    return r0;
}

static struct U5HudShared *diagnostic_mapping(struct ProbeState *s) {
    struct U5HudShared *shared = __atomic_load_n(&s->shared, __ATOMIC_ACQUIRE);
    if (shared) return shared;
    int fd = linux_call(5, (long)U5HUD_STATE_PATH, 2, 0, 0, 0, 0);
    if (fd < 0) return 0;
    unsigned magic = 0;
    int count = linux_call(3, fd, (long)&magic, sizeof(magic), 0, 0, 0);
    if (count != sizeof(magic) || magic != U5HUD_MAGIC) {
        linux_call(6, fd, 0, 0, 0, 0, 0);
        return 0;
    }
    long address = linux_call(192, 0, U5HUD_STATE_BYTES, 3, 1, fd, 0);
    linux_call(6, fd, 0, 0, 0, 0, 0);
    if ((unsigned long)address >= (unsigned long)-4095) return 0;
    shared = (struct U5HudShared *)address;
    struct U5HudShared *expected = 0;
    s->pid = linux_call(20, 0, 0, 0, 0, 0, 0);
    if (!__atomic_compare_exchange_n(&s->shared, &expected, shared, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        linux_call(91, address, U5HUD_STATE_BYTES, 0, 0, 0, 0);
        shared = expected;
    }
    return shared;
}

#include "nav_producer.h"
#include "trace.h"

static unsigned hex_digit(unsigned c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 256;
}

static unsigned nmea_fix(const unsigned char *p, unsigned n) {
    for (unsigned start = 0; start + 10 < n; ++start) {
        if (p[start] != '$' || p[start + 1] != 'G' ||
            (p[start + 2] != 'P' && p[start + 2] != 'N') ||
            p[start + 3] != 'R' || p[start + 4] != 'M' || p[start + 5] != 'C' ||
            p[start + 6] != ',') continue;
        unsigned checksum = 0, commas = 0, status = 0;
        for (unsigned i = start + 1; i + 2 < n && i - start < 128; ++i) {
            if (p[i] == '*') {
                unsigned high = hex_digit(p[i + 1]), low = hex_digit(p[i + 2]);
                if (high < 16 && low < 16 && checksum == ((high << 4) | low)) return status;
                break;
            }
            checksum ^= p[i];
            if (p[i] == ',') {
                ++commas;
                if (commas == 2) {
                    status = p[i + 1] == 'A' ? 1 : p[i + 1] == 'V' ? 2 : 0;
                }
            }
        }
    }
    return 0;
}
