#define _POSIX_C_SOURCE 200809L
#include "u5hud_shared.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#include "../ble-r18/ble_shared.h"

static const char *state_failure = "none";
static int state_errno;

static struct U5HudShared *state_error(const char *stage, int code) {
    state_failure = stage;
    state_errno = code;
    return NULL;
}

static int memory_filesystem(unsigned type) {
    return type == 0x01021994u || type == 0x858458f6u;
}

static int prepare_storage(void) {
    struct statfs storage;
    if (mkdir(U5HUD_RAM_DIR, 0700) != 0 && errno != EEXIST) {
        state_error("ram_directory", errno);
        return 0;
    }
    if (statfs(U5HUD_RAM_DIR, &storage) != 0) {
        state_error("ram_filesystem", errno);
        return 0;
    }
    if (!memory_filesystem((unsigned)storage.f_type)) {
        /* Some builds keep /tmp on the root filesystem. Mount a private RAM
         * directory instead of ever putting diagnostic records on flash. */
        unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC;
        if (mount("u5hud", U5HUD_RAM_DIR, "tmpfs", flags, "size=2097152,mode=0700,nr_inodes=8") != 0 &&
            mount("u5hud", U5HUD_RAM_DIR, "ramfs", flags, "mode=0700") != 0) {
            state_error("ram_mount", errno);
            return 0;
        }
        if (statfs(U5HUD_RAM_DIR, &storage) != 0 ||
            !memory_filesystem((unsigned)storage.f_type)) {
            state_error("ram_backing_unconfirmed", EINVAL);
            return 0;
        }
    }
    return 1;
}

static unsigned uptime_seconds(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? (unsigned)now.tv_sec : 0;
}

static int saved_enabled(void) {
    char value = '1';
    int fd = open(U5HUD_CONFIG_PATH, O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &value, 1) != 1) value = '1';
        close(fd);
    }
    return value != '0';
}

static struct U5HudShared *map_state(int writable, int initialize) {
    if (initialize && !prepare_storage()) return NULL;
    int flags = writable ? O_RDWR : O_RDONLY;
    int fd = open(U5HUD_STATE_PATH, flags | (initialize ? O_CREAT : 0), 0600);
    if (fd < 0) return state_error("state_open", errno);
    if (initialize && flock(fd, LOCK_EX) != 0) {
        int failure = errno;
        close(fd);
        return state_error("state_lock", failure);
    }
    /* Only the size of our own regular state file is needed here. Modern
     * 32-bit musl may implement fstat through statx; the target Tina kernel
     * returns EINVAL on that path. llseek works with its original kernel ABI. */
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        int failure = errno;
        close(fd);
        return state_error("state_seek", failure);
    }
    if (size != U5HUD_STATE_BYTES &&
        (!initialize || ftruncate(fd, U5HUD_STATE_BYTES) != 0)) {
        int failure = initialize ? errno : EINVAL;
        close(fd);
        return state_error("state_size", failure);
    }
    unsigned old_magic = 0;
    if (size == U5HUD_STATE_BYTES &&
        (lseek(fd, 0, SEEK_SET) < 0 || read(fd, &old_magic, sizeof(old_magic)) != sizeof(old_magic))) {
        int failure = errno;
        close(fd);
        return state_error("state_header", failure);
    }
    if (!initialize && old_magic != U5HUD_MAGIC) {
        close(fd); return state_error("state_magic", EINVAL);
    }
    int reset = initialize && (size != U5HUD_STATE_BYTES || old_magic != U5HUD_MAGIC);
    if (reset) {
        /* Allocate RAM pages using checked writes before mmap. A sparse tmpfs
         * file can otherwise raise SIGBUS on a small/full mount during memset. */
        static const unsigned char zeros[U5HUD_PAGE_SIZE];
        if (lseek(fd, 0, SEEK_SET) < 0) {
            int failure = errno; close(fd); return state_error("state_allocate", failure);
        }
        for (unsigned used = 0; used < U5HUD_STATE_BYTES;) {
            ssize_t written = write(fd, zeros, sizeof(zeros));
            if (written <= 0) {
                int failure = written < 0 ? errno : EIO;
                close(fd); return state_error("state_allocate", failure);
            }
            used += (unsigned)written;
            if ((unsigned)written != sizeof(zeros)) {
                close(fd); return state_error("state_allocate", ENOSPC);
            }
        }
    }
    int protection = PROT_READ | (writable ? PROT_WRITE : 0);
    struct U5HudShared *shared = mmap(NULL, U5HUD_STATE_BYTES, protection, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) {
        int failure = errno;
        close(fd);
        return state_error("state_map", failure);
    }
    if (reset) {
        shared->enabled = saved_enabled();
        shared->cleared_seconds = uptime_seconds();
        if (shared->enabled) {
            struct timespec now = {0};
            clock_gettime(CLOCK_MONOTONIC, &now);
            u5hud_new_event(shared, U5_INIT, (unsigned)now.tv_sec, now.tv_nsec / 1000000);
        }
        __atomic_store_n(&shared->magic, U5HUD_MAGIC, __ATOMIC_RELEASE);
    }
    close(fd);
    if (__atomic_load_n(&shared->magic, __ATOMIC_ACQUIRE) != U5HUD_MAGIC) {
        munmap(shared, U5HUD_STATE_BYTES);
        return state_error("state_magic", EINVAL);
    }
    return shared;
}

static void reset_trace(struct U5HudShared *shared) {
    unsigned session = shared->trace_session;
    unsigned ready = shared->protocol_ready;
    unsigned pass_known = shared->native_pass_known, pass_enabled = shared->native_pass_enabled;
    unsigned dispatch = shared->accepted_dispatch_complete, callback_known = shared->callback_known;
    unsigned callback_present = shared->callback_present, trigger = shared->subscription_trigger;
    unsigned attempted = shared->subscription_attempted;
    memset(&shared->trace_next, 0, sizeof(*shared) - ((unsigned char *)&shared->trace_next - (unsigned char *)shared));
    shared->trace_session = session;
    shared->protocol_ready = ready;
    shared->native_pass_known = pass_known; shared->native_pass_enabled = pass_enabled;
    shared->accepted_dispatch_complete = dispatch; shared->callback_known = callback_known;
    shared->callback_present = callback_present; shared->subscription_trigger = trigger;
    shared->subscription_attempted = attempted;
    memset((unsigned char *)shared + U5HUD_PAGE_SIZE, 0, U5HUD_STATE_BYTES - U5HUD_PAGE_SIZE);
}

static void control_event(struct U5HudShared *shared, unsigned kind) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    u5hud_new_event(shared, kind, (unsigned)now.tv_sec, now.tv_nsec / 1000000);
}

static int lock_state(struct U5HudShared *shared) {
    const struct timespec delay = {0, 1000000};
    unsigned pid = (unsigned)getpid();
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (u5hud_try_lock(shared, pid)) {
            /* Recover a writer interrupted by process termination. */
            if (shared->sequence & 1) {
                memset(&shared->diagnostics, 0, sizeof(shared->diagnostics));
                reset_trace(shared);
                ++shared->generation;
                shared->cleared_seconds = uptime_seconds();
                ++shared->sequence;
            }
            u5hud_write_begin(shared);
            return 1;
        }
        unsigned owner = __atomic_load_n(&shared->owner, __ATOMIC_RELAXED);
        if (owner && kill((pid_t)owner, 0) != 0 && errno == ESRCH) {
            __atomic_compare_exchange_n(&shared->owner, &owner, 0, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        }
        nanosleep(&delay, NULL);
    }
    return 0;
}

static int read_snapshot(const struct U5HudShared *shared, struct U5HudShared *copy) {
    if (!shared) return 0;
    const struct timespec pause = {0, 1000000};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        unsigned before = __atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE);
        if (before & 1) { nanosleep(&pause, NULL); continue; }
        memcpy(copy, shared, sizeof(*copy));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before == __atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE)) return 1;
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int live_process(unsigned pid, const char *wanted) {
    if (!pid) return 0;
    char path[96], name[64] = {0}, line[256], *end = NULL;
    snprintf(path, sizeof(path), "/proc/%u/comm", pid);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    if (fgets(name, sizeof(name), file)) name[strcspn(name, "\r\n")] = 0;
    fclose(file);
    if (strcmp(name, wanted) != 0) return 0;
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    file = fopen(path, "r");
    if (file) {
        if (fgets(line, sizeof(line), file)) end = strrchr(line, ')');
        fclose(file);
    }
    return end && end[1] == ' ' && end[2] && end[2] != 'Z' && end[2] != 'X';
}

static unsigned process_id(const char *wanted, unsigned preferred) {
    /* The stock executable forks a supervisor and worker with the same name.
     * Validate the session writer first, rather than choosing the first name. */
    if (live_process(preferred, wanted)) return preferred;
    DIR *directory = opendir("/proc");
    if (!directory) return 0;
    struct dirent *entry;
    unsigned found = 0;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        unsigned candidate = (unsigned)strtoul(entry->d_name, NULL, 10);
        if (live_process(candidate, wanted)) { found = candidate; break; }
    }
    closedir(directory);
    return found;
}

struct Connection {
    unsigned app_pid, renderer_pid, marker, known, handshake, confirmed;
};

static struct Connection connection(const struct U5HudShared *copy, int available) {
    struct Connection result = {0};
    result.app_pid = process_id("lylinkapp", available ? copy->session_pid : 0);
    result.renderer_pid = process_id("rcpservice", 0);
    result.marker = access("/tmp/lywcpok", F_OK) == 0;
    result.known = available && result.app_pid && copy->session_pid == result.app_pid;
    result.handshake = result.known && copy->session_accepted;
    unsigned now = uptime_seconds();
    unsigned accepted = available ? copy->session_accepted_seconds : 0;
    /* The renderer PID is an observation, not a protocol-session requirement.
     * A real connected box reported PID 0, so do not gate CarPlay on its name. */
    result.confirmed = result.handshake && result.marker &&
                       (!copy->native_pass_known || copy->native_pass_enabled) &&
                       now >= accepted && now - accepted >= 2;
    return result;
}

static void headers(const char *type) {
    printf("Content-Type: %s; charset=utf-8\r\nCache-Control: no-store\r\n"
           "X-Content-Type-Options: nosniff\r\n", type);
}

static void json_error(const char *status, const char *reason) {
    printf("Status: %s\r\n", status);
    headers("application/json");
    printf("\r\n{\"ok\":false,\"error\":\"%s\"}\n", reason);
}

#include "u5hud_export.h"

static void status_response(void) {
    struct U5HudShared *shared = map_state(0, 0), copy;
    int available = read_snapshot(shared, &copy);
    if (shared && !available) state_error("snapshot_busy", EBUSY);
    struct Connection current = connection(&copy, available);
    headers("application/json");
    printf("\r\n{\"ok\":true,\"session_known\":%s,\"carplay\":%s,"
           "\"service_running\":%s,"
           "\"logging\":%s,\"diagnostics_available\":%s,\"build\":\"%s\","
           "\"app_pid\":%u,\"renderer_pid\":%u,\"session_marker\":%s,\"iap_session_accepted\":%s,"
           "\"session_pid\":%u,\"protocol_callback_completed\":%s,"
           "\"native_pass_state\":%s,\"identification_dispatch_completed\":%s,"
           "\"native_success_callback_present\":%s,\"subscription_attempted\":%s,\"subscription_trigger\":\"%s\","
           "\"state_error\":\"%s\",\"state_errno\":%d,\"log_capacity_bytes\":%u,"
           "\"trace_count\":%u,\"trace_capacity\":%u,\"trace_overwritten\":%u,\"trace_dropped\":%u}\n",
           current.known ? "true" : "false", current.confirmed ? "true" : "false",
           current.app_pid ? "true" : "false",
           shared ? (__atomic_load_n(&shared->enabled, __ATOMIC_ACQUIRE) ? "true" : "false") : "null",
           available ? "true" : "false", U5HUD_BUILD,
           current.app_pid, current.renderer_pid, current.marker ? "true" : "false",
           current.known ? (current.handshake ? "true" : "false") : "null",
           available ? copy.session_pid : 0, available ? (copy.protocol_ready ? "true" : "false") : "null",
           available && copy.native_pass_known ? (copy.native_pass_enabled ? "true" : "false") : "null",
           available ? (copy.accepted_dispatch_complete ? "true" : "false") : "null",
           available && copy.callback_known ? (copy.callback_present ? "true" : "false") : "null",
           available ? (copy.subscription_attempted ? "true" : "false") : "null",
           subscription_source(available ? copy.subscription_trigger : 0),
           state_failure, state_errno,
           U5HUD_STATE_BYTES, available ? copy.trace_count : 0, U5HUD_EVENT_CAPACITY,
           available ? copy.trace_overwritten : 0, available ? copy.trace_dropped : 0);
    if (shared) munmap(shared, U5HUD_STATE_BYTES);
}

static void download_response(void) {
    struct U5HudShared *shared = map_state(0, 0), copy;
    int available = read_snapshot(shared, &copy);
    unsigned char *snapshot = available ? trace_snapshot(shared) : NULL;
    if (snapshot) memcpy(&copy, snapshot, sizeof(copy));
    if (shared && !available) state_error("snapshot_busy", EBUSY);
    headers("text/plain");
    printf("Content-Disposition: attachment; filename=\"u5hud-log.txt\"\r\n\r\n"
           "build=%s\nuptime_seconds=%u\n", U5HUD_BUILD, uptime_seconds());
    struct Connection current = connection(&copy, available);
    printf("app_pid=%u\nrenderer_pid=%u\ncarplay_session_marker=%u\niap_session_accepted=%s\n"
           "carplay_connected=%u\nstate_path=%s\nstate_error=%s\nstate_errno=%d\n",
           current.app_pid, current.renderer_pid, current.marker,
           current.known ? (current.handshake ? "1" : "0") : "unknown",
           current.confirmed, U5HUD_STATE_PATH, state_failure, state_errno);
    printf("session_pid=%u\nsession_process_matches=%u\nprotocol_callback_completed=%s\n",
           available ? copy.session_pid : 0, current.known,
           available ? (copy.protocol_ready ? "1" : "0") : "unknown");
    printf("native_pass_state=%s\n", available && copy.native_pass_known ?
           (copy.native_pass_enabled ? "1" : "0") : "unknown");
    if (available) printf("identification_dispatch_completed=%u\nnative_success_callback_present=%s\n"
                          "subscription_attempted=%u\nsubscription_trigger=%s\n",
                          copy.accepted_dispatch_complete, copy.callback_known ? (copy.callback_present ? "1" : "0") : "unknown",
                          copy.subscription_attempted, subscription_source(copy.subscription_trigger));
    struct statfs storage;
    if (statfs(U5HUD_RAM_DIR, &storage) == 0)
        printf("diagnostic_filesystem=0x%08x\n", (unsigned)storage.f_type);
    else printf("diagnostic_filesystem_errno=%d\n", errno);
    if (available) {
        struct U5HudDiagnostics *d = &copy.diagnostics;
        printf("logging_enabled=%u\ngeneration=%u\ncleared_uptime_seconds=%u\n"
               "identify=%u\naccepted=%u\nrejected=%u\nsubscribe=%u\nsubscribe_return=%d\n"
               "rx_5201=%u\nrx_5202=%u\nrx_5204=%u\n"
               "gps_requests=%u\ngps_stops=%u\ngps_received_from_hu=%u\n"
               "gps_last_uptime_seconds=%u\ngps_fix=%s\n",
               copy.enabled, copy.generation, copy.cleared_seconds,
               d->identify, d->accepted, d->rejected, d->subscribe, d->subscribe_result,
               d->rx_5201, d->rx_5202, d->rx_5204, d->gps_requests, d->gps_stops,
               d->gps_received, d->gps_last_seconds,
               d->gps_fix == 1 ? "valid" : d->gps_fix == 2 ? "invalid" : "unknown");
        printf("schema_version=6\nlog_capacity_bytes=%u\ntrace_count=%u\ntrace_capacity=%u\n"
               "trace_overwritten=%u\ntrace_dropped=%u\ntrace_truncated=%u\n"
               "nav_parse_ok=%u\nnav_parse_bad=%u\nnav_unknown_fields=%u\nnav_shape_warnings=%u\n"
               "nav_last_uptime_seconds=%u\nnav_last_message=0x%04x\nrx_pending_bytes=%u\nrx_expected_bytes=%u\n",
               U5HUD_STATE_BYTES, copy.trace_count,U5HUD_EVENT_CAPACITY,copy.trace_overwritten,copy.trace_dropped,
               copy.trace_truncated,copy.nav_parse_ok,copy.nav_parse_bad,copy.nav_unknown,copy.nav_shape_warnings,
               copy.nav_last_seconds,copy.nav_last_message,copy.rx_pending_bytes,copy.rx_expected_bytes);
    } else {
        printf("diagnostics=unavailable\n");
    }
    if (snapshot) { dump_trace(snapshot); free(snapshot); }
    else printf("timeline_snapshot=unavailable_or_busy\n");
    ble_dump();
    FILE *memory = fopen("/proc/meminfo", "r");
    if (memory) {
        char line[128];
        while (fgets(line, sizeof(line), memory)) {
            if (strncmp(line, "MemTotal:", 9) == 0 || strncmp(line, "MemAvailable:", 13) == 0)
                fputs(line, stdout);
        }
        fclose(memory);
    }
    if (shared) munmap(shared, U5HUD_STATE_BYTES);
}

static int persist_enabled(unsigned enabled) {
    char temporary[128];
    snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", U5HUD_CONFIG_PATH, (long)getpid());
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return 0;
    const char *value = enabled ? "1\n" : "0\n";
    int ok = write(fd, value, 2) == 2 && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (ok) ok = rename(temporary, U5HUD_CONFIG_PATH) == 0;
    if (ok) {
        int directory = open("/mnt/UDISK", O_RDONLY | O_DIRECTORY);
        if (directory >= 0) {
            fsync(directory);
            close(directory);
        }
    }
    if (!ok) unlink(temporary);
    return ok;
}

static void control_error(const char *reason) {
    /* Keep application failures as JSON results, like the stock settings CGI.
     * Method/body validation still uses HTTP errors before reaching here. */
    headers("application/json");
    printf("\r\n{\"ok\":false,\"error\":\"%s\",\"stage\":\"%s\",\"errno\":%d}\n",
           reason, state_failure, state_errno);
}

static void control_response(const char *action) {
    struct U5HudShared *shared = map_state(1, 1);
    if (!shared) {
        control_error("diagnostics_unavailable");
        return;
    }
    if (!lock_state(shared)) {
        munmap(shared, U5HUD_STATE_BYTES);
        state_error("state_busy", EBUSY);
        control_error("diagnostics_busy");
        return;
    }
    int ok = 1;
    if (strcmp(action, "clear") == 0) {
        /* Remove the earlier R1 snapshot, if present. The shared page stays mapped. */
        if (unlink("/tmp/u5hud_diag") != 0 && errno != ENOENT) ok = 0;
        if (ok) {
            memset(&shared->diagnostics, 0, sizeof(shared->diagnostics));
            reset_trace(shared);
            ble_mark_clear();
            shared->cleared_seconds = uptime_seconds();
            __atomic_add_fetch(&shared->generation, 1, __ATOMIC_RELEASE);
            if (shared->enabled) control_event(shared, U5_CLEAR);
        }
    } else {
        unsigned enabled = strcmp(action, "enable") == 0;
        if (shared->enabled != enabled) {
            ok = persist_enabled(enabled);
            if (ok) {
                control_event(shared, enabled ? U5_LOG_ENABLED : U5_LOG_DISABLED);
                __atomic_add_fetch(&shared->generation, 1, __ATOMIC_RELEASE);
                shared->rx_pending_bytes = shared->rx_expected_bytes = 0;
                __atomic_store_n(&shared->enabled, enabled, __ATOMIC_RELEASE);
            }
        }
    }
    u5hud_unlock(shared);
    munmap(shared, U5HUD_STATE_BYTES);
    if (!ok) {
        state_error("settings_write", errno);
        control_error("settings_write_failed");
        return;
    }
    headers("application/json");
    printf("\r\n{\"ok\":true}\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--init") == 0) {
        struct U5HudShared *shared = map_state(1, 1);
        if (!shared) {
            fprintf(stderr, "u5hud initialization failed: %s errno=%d\n", state_failure, state_errno);
            return 1;
        }
        munmap(shared, U5HUD_STATE_BYTES);
        return 0;
    }
    const char *method = getenv("REQUEST_METHOD");
    const char *query = getenv("QUERY_STRING");
    const char *action = query ? query : "action=status";
    if (argc == 2 && strcmp(argv[1], "--download") == 0) action = "action=download";
    if (strcmp(action, "action=status") == 0 || strcmp(action, "action=download") == 0) {
        if (method && strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
            json_error("405 Method Not Allowed", "get_required");
            return 0;
        }
        if (strcmp(action, "action=status") == 0) status_response();
        else download_response();
        return 0;
    }
    if (strcmp(action, "action=enable") != 0 && strcmp(action, "action=disable") != 0 &&
        strcmp(action, "action=clear") != 0) {
        json_error("400 Bad Request", "unknown_action");
        return 0;
    }
    if (!method || strcmp(method, "POST") != 0) {
        json_error("405 Method Not Allowed", "post_required");
        return 0;
    }
    const char *type = getenv("CONTENT_TYPE"), *length = getenv("CONTENT_LENGTH");
    char body[2];
    if (!type || strcmp(type, "application/json") != 0 || !length || strcmp(length, "2") != 0 ||
        fread(body, 1, sizeof(body), stdin) != sizeof(body) || memcmp(body, "{}", 2) != 0) {
        json_error("403 Forbidden", "json_control_body_required");
        return 0;
    }
    control_response(action + 7);
    return 0;
}
