/* Included by the single BLE worker. The diagnostic sockets only observe events. */
enum {
    HCI_EVENT_PACKET = 0x04, HCI_DISCONNECTION_COMPLETE = 0x05, HCI_HARDWARE_ERROR = 0x10, HCI_LE_META = 0x3e,
    HCI_LE_CONNECTION_COMPLETE = 0x01, HCI_LE_ENHANCED_CONNECTION_COMPLETE = 0x0a,
    HCI_LE_CONNECTION_UPDATE_COMPLETE = 0x03,
    MGMT_DEVICE_CONNECTED = 0x0b, MGMT_DEVICE_DISCONNECTED = 0x0c
};

static struct {
    struct BlePeer peer;
    unsigned attempt, controller;
    int handle;
    int closed, had_link, mgmt_disconnected, mgmt_ambiguous, hci_tried, navigation_recorded;
    int64_t last_write_ms;
    int64_t last_navigation_write_ms;
} link_watch;
static int link_hci = -1;
static unsigned link_hci_controller;

static uint32_t link_write_age(int64_t now, int64_t timestamp) {
    int64_t age = now - timestamp;
    return timestamp && age >= 0 && age < UINT32_MAX ? (uint32_t)age : UINT32_MAX;
}

static void link_note(unsigned attempt, unsigned kind, int first, int second, int third) {
    int saved = errno;
    if (logging_enabled()) {
        int64_t now = ble_now();
        struct BleLinkEvent *e = &view.link_events[view.link_event_next++ % BLE_LINK_EVENTS];
        *e = (struct BleLinkEvent){
            .seconds = (uint32_t)(now / 1000), .milliseconds = (uint16_t)(now % 1000),
            .attempt = attempt, .kind = (uint16_t)kind, .values = {first, second, third},
            .last_write_age_ms = link_write_age(now, attempt ? link_watch.last_write_ms : 0),
            .last_navigation_write_age_ms = link_write_age(now, attempt ? link_watch.last_navigation_write_ms : 0)
        };
        if (view.link_event_count < BLE_LINK_EVENTS) ++view.link_event_count;
    }
    errno = saved;
}

static int link_matches(const unsigned char *address, unsigned type) {
    return type == link_watch.peer.type && !memcmp(address, link_watch.peer.address, 6);
}

static void link_mgmt_event(unsigned code, unsigned index, const unsigned char *p, unsigned n) {
    if (code != MGMT_DEVICE_CONNECTED && code != MGMT_DEVICE_DISCONNECTED) return;
    if (!view.controller_known || index != view.controller_index || n < 7 || p[6] > 2) return;
    if (code == MGMT_DEVICE_CONNECTED && (n < 13 || le16(p + 11) != n - 13)) return;
    if (!link_watch.attempt || index != link_watch.controller || !link_matches(p, p[6])) {
        link_note(link_watch.attempt, BL_OTHER_PEER, p[6], code == MGMT_DEVICE_CONNECTED,
                  code == MGMT_DEVICE_DISCONNECTED && n >= 8 ? p[7] : -1);
        return;
    }
    if (code == MGMT_DEVICE_CONNECTED) {
        if (!link_watch.closed) {
            link_watch.had_link = 1;
            link_watch.mgmt_disconnected = link_watch.mgmt_ambiguous = 0;
        }
        return;
    }
    /* MGMT has no handle. If a previous link to this address is still unresolved,
     * leave the attempt unknown until the new Device Connected event is seen. */
    unsigned attempt = link_watch.mgmt_ambiguous ? 0 : link_watch.attempt;
    link_note(attempt, BL_MGMT_DISCONNECT, n >= 8 ? p[7] : -1, attempt ? link_watch.closed : -1, 0);
    if (attempt) link_watch.mgmt_disconnected = 1;
}

static void link_hci_event(const unsigned char *p, unsigned n) {
    if (n < 3 || p[0] != HCI_EVENT_PACKET || p[2] != n - 3) return;
    if (p[1] == HCI_DISCONNECTION_COMPLETE && n == 7 && p[3] == 0 && link_watch.handle == (int)le16(p + 4)) {
        link_note(link_watch.attempt, BL_HCI_DISCONNECT, link_watch.handle, p[6], link_watch.closed);
        link_watch.handle = -1;
        return;
    }
    if (p[1] == HCI_HARDWARE_ERROR && n == 4) {
        link_note(link_watch.attempt, BL_HCI_HARDWARE_ERROR, p[3], 0, 0);
        return;
    }
    if (p[1] != HCI_LE_META || link_watch.closed || n < 4) return;
    if (p[3] == HCI_LE_CONNECTION_UPDATE_COMPLETE) {
        if (n != 13 || link_watch.handle != (int)le16(p + 5)) return;
        if (p[4]) link_note(link_watch.attempt, BL_HCI_PARAMS_REJECTED, link_watch.handle, p[4], 0);
        else link_note(link_watch.attempt, BL_HCI_PARAMS_UPDATED, le16(p + 7), le16(p + 9), le16(p + 11));
        return;
    }
    if (n < 22 || p[4] != 0 || p[7] != 0) return;
    int enhanced = p[3] == HCI_LE_ENHANCED_CONNECTION_COMPLETE && n == 34;
    if (!enhanced && (p[3] != HCI_LE_CONNECTION_COMPLETE || n != 22)) return;
    unsigned handle = le16(p + 5);
    if (handle > 0x0eff || p[8] > (enhanced ? 3 : 1)) return;
    static const unsigned char zero_address[6];
    if (!link_matches(p + 9, 1 + (p[8] & 1)) &&
        !(enhanced && memcmp(p + 21, zero_address, 6) && link_matches(p + 21, 2))) return;
    link_watch.handle = (int)handle;
    link_watch.had_link = 1;
    link_note(link_watch.attempt, BL_HCI_HANDLE, (int)handle, 0, 0);
    const unsigned char *params = p + (enhanced ? 27 : 15);
    link_note(link_watch.attempt, BL_HCI_PARAMS, le16(params), le16(params + 2), le16(params + 4));
}

static void link_hci_close(void) {
    if (link_hci >= 0) close(link_hci);
    link_hci = -1;
    link_watch.handle = -1;
}

static void link_open_hci(void) {
    if (link_hci >= 0 && link_hci_controller != link_watch.controller) link_hci_close();
    if (link_hci >= 0 || link_watch.closed || link_watch.hci_tried) return;
    link_watch.hci_tried = 1;
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 1);
    if (fd < 0) {
        link_note(link_watch.attempt, BL_HCI_MONITOR, BL_HCI_SOCKET, errno, 0);
        return;
    }
    /* Linux HCI_FILTER uses the 32-bit userspace layout. */
    const struct { uint32_t type_mask, event_mask[2]; uint16_t opcode, padding; } filter = {
        1u << HCI_EVENT_PACKET, {(1u << HCI_DISCONNECTION_COMPLETE) | (1u << HCI_HARDWARE_ERROR),
                                 1u << (HCI_LE_META - 32)}, 0, 0
    };
    const struct HciAddress address = {AF_BLUETOOTH, link_watch.controller, 0};
    if (setsockopt(fd, 0 /* SOL_HCI */, 2 /* HCI_FILTER */, &filter, sizeof(filter))) {
        link_note(link_watch.attempt, BL_HCI_MONITOR, BL_HCI_FILTER, errno, 0);
        close(fd);
    } else if (bind(fd, (const struct sockaddr *)&address, sizeof(address))) {
        link_note(link_watch.attempt, BL_HCI_MONITOR, BL_HCI_BIND, errno, 0);
        close(fd);
    } else {
        link_hci = fd;
        link_hci_controller = link_watch.controller;
        link_note(link_watch.attempt, BL_HCI_MONITOR, BL_HCI_LISTENING, 0, 0);
    }
}

static void link_poll_events(void) {
    if (!link_watch.attempt) return;
    int saved = errno;
    if (!logging_enabled()) {
        link_hci_close();
        link_watch.hci_tried = 0;
        errno = saved;
        return;
    }
    link_open_hci();
    unsigned char packet[512];
    for (unsigned i = 0; link_hci >= 0 && i < 16; ++i) {
        ssize_t n = recv(link_hci, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                link_note(link_watch.attempt, BL_HCI_MONITOR, BL_HCI_READ, n == 0 ? EIO : errno, 0);
                link_hci_close();
            }
            break;
        }
        if ((size_t)n <= sizeof(packet)) link_hci_event(packet, (unsigned)n);
    }
    /* Called outside management commands. Their synchronous receive loop also
     * passes events to link_mgmt_event, so command replies remain with it. */
    for (unsigned i = 0; mgmt >= 0 && i < 16; ++i) {
        ssize_t n = recv(mgmt, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (n >= 6 && (size_t)n <= sizeof(packet) && le16(packet + 4) == (unsigned)n - 6)
            link_mgmt_event(le16(packet), le16(packet + 2), packet + 6, (unsigned)n - 6);
    }
    errno = saved;
}

static void link_begin(const struct BlePeer *peer) {
    link_poll_events();
    int ambiguous = link_watch.attempt && link_watch.controller == view.controller_index &&
        link_matches(peer->address, peer->type) && !link_watch.mgmt_disconnected &&
        (link_watch.had_link || link_watch.mgmt_ambiguous);
    link_watch.peer = *peer;
    link_watch.attempt = ++view.link_last_attempt;
    link_watch.controller = view.controller_index;
    link_watch.handle = -1;
    link_watch.closed = link_watch.had_link = link_watch.mgmt_disconnected = link_watch.hci_tried = 0;
    link_watch.mgmt_ambiguous = ambiguous;
    link_watch.last_write_ms = 0;
    link_watch.last_navigation_write_ms = 0;
    link_watch.navigation_recorded = 0;
    link_note(link_watch.attempt, BL_ATTEMPT, 0, 0, 0);
    link_poll_events();
}

static void link_poll_error(short revents) {
    int saved = errno;
    if (link_watch.attempt && logging_enabled()) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(att, SOL_SOCKET, SO_ERROR, &error, &size)) error = -errno;
        else if (size != sizeof(error)) error = -EPROTO;
        link_note(link_watch.attempt, BL_POLL_ERROR, revents, error, 0);
    }
    errno = saved;
}

static void link_io_error(unsigned operation, int error, int result) {
    if (link_watch.attempt) link_note(link_watch.attempt, BL_IO_ERROR, (int)operation, error, result);
}

static void link_written(unsigned bytes, int response, int cleanup) {
    int saved = errno;
    if (!cleanup && link_watch.attempt && logging_enabled()) {
        if (!link_watch.last_write_ms) link_note(link_watch.attempt, BL_FIRST_WRITE, (int)bytes, response, 0);
        link_watch.last_write_ms = ble_now();
    }
    errno = saved;
}

static void link_navigation_written(unsigned icon, unsigned distance, int response) {
    if (!link_watch.attempt || link_watch.navigation_recorded || !logging_enabled()) return;
    link_watch.navigation_recorded = 1;
    link_note(link_watch.attempt, BL_FIRST_NAVIGATION, (int)icon, (int)distance, response);
}
