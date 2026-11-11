/* Optional OoT Editor bridge, update 131072 only. Included once by game_patches_ns2.c.
   Commands are queued by the local input reader and consumed by the game's controller-poll
   thread. No external process memory access, scanning, or save-file writes.
   Layout leads: Kabosune/oot-remake-modding docs/address-map.md and sloperina-findings.md;
   offsets below are MODULE offsets (their absolute addresses minus 0x80020000).
   Confirmed against the player's original main.elf before use. Implementation is our own. */
#include <ctype.h>
#include <fcntl.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <sys/stat.h>

enum { OOT_LIVE_ACTORS = 0x4e90d90, OOT_LIVE_CCT_VT = 0x4c34f10,
       OOT_LIVE_BODY_VT = 0x4c34658, OOT_LIVE_ACTOR_VT = 0x4b25e18,
       OOT_LIVE_GD = 0x4c7e180, OOT_LIVE_EVENT = 0x4c7e090 };
static uintptr_t oot_live_main;
static int oot_live_fd = -1;
static pthread_mutex_t oot_live_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind, destination; char id[40], map[64]; float xyz[4]; } oot_live_request;
static _Atomic int oot_live_pending;
static int oot_live_check_frames;
static float oot_live_check_xyz[3];
static char oot_live_check_map[64];
static uint64_t oot_live_deadline;
static uint64_t oot_live_queue_deadline;
static uint64_t oot_live_ready_since, oot_live_started, oot_live_next_check;
static int oot_live_read(uintptr_t at, void *out, size_t n);
static uintptr_t oot_live_ptr(uintptr_t at);

/* Read the String64 hash table, rather than baking save-field array indices into the bridge. */
static int oot_live_string(uint32_t hash, char out[64]) {
    uintptr_t mgr = oot_live_ptr(oot_live_ptr(oot_live_main + OOT_LIVE_GD));
    uint32_t count = 0, capacity = 0;
    uintptr_t type = mgr + 0x608, objects = oot_live_ptr(type + 0x10), table = oot_live_ptr(type + 0x18);
    if (!mgr || !objects || !table || !oot_live_read(type + 8, &count, 4) || count > 256 ||
        !oot_live_read(type + 0x24, &capacity, 4) || capacity > 4096) return 0;
    for (uint32_t i = 0; i < capacity; i++) {
        uint32_t pair[2]; if (!oot_live_read(table + i * 8, pair, sizeof pair)) return 0;
        if (pair[0] != hash || pair[1] >= count) continue;
        uintptr_t obj = objects + pair[1] * 0x108;
        uint32_t n = 0; uintptr_t value = oot_live_ptr(obj + 0x68);
        if (!oot_live_read(obj + 0x70, &n, 4) || n < 64 || n > 1024 || !oot_live_read(value, out, 64)) return 0;
        if (!memchr(out, 0, 64)) return 0;
        for (unsigned j = 0; out[j]; j++) if (!isalnum((unsigned char)out[j]) && out[j] != '_') return 0;
        return 1;
    }
    return 0;
}
static int oot_live_paused(void) {
    uintptr_t mgr = oot_live_ptr(oot_live_main + 0x4e97e38), rows = oot_live_ptr(mgr + 8);
    uint32_t n = 0;
    if (!mgr || !rows || !oot_live_read(mgr, &n, 4) || n > 256) return 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t hash = 0, count = 0;
        if (!oot_live_read(rows + i * 32 + 8, &hash, 4) || !oot_live_read(rows + i * 32 + 0x1c, &count, 4)) return 1;
        if ((hash == 0xd468c7d1 || hash == 0xba597383) && (count & 0x7fffffff)) return 1;
    }
    return 0;
}

static int oot_live_read(uintptr_t at, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return at > 0x10000 && at <= UINTPTR_MAX - n &&
        mach_vm_read_overwrite(mach_task_self(), at, n, (uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}
static uintptr_t oot_live_ptr(uintptr_t at) {
    uintptr_t p = 0; return oot_live_read(at, &p, sizeof p) ? p : 0;
}
static int oot_live_xyz(const float *v) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) &&
        fabsf(v[0]) < 10000 && fabsf(v[1]) < 10000 && fabsf(v[2]) < 10000;
}
static void oot_live_reply(const char *id, const char *status, const float *xyz) {
    char map[64] = {0}; if (oot_live_main && !oot_live_string(0x1d6189da, map)) map[0] = 0;
    char line[320];
    int n = snprintf(line, sizeof line,
        "{\"id\":\"%s\",\"status\":\"%s\",\"map\":\"%s\",\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}\n",
        id, status, map, xyz ? xyz[0] : 0, xyz ? xyz[1] : 0, xyz ? xyz[2] : 0);
    if (n > 0 && (size_t)n < sizeof line && oot_live_fd >= 0) {
        /* One write per small record; input and game threads can both report safely. */
        ssize_t result = write(oot_live_fd, line, (size_t)n); (void)result;
    }
    fprintf(stderr, "[oot-live] %s %s %.3f %.3f %.3f\n", id, status,
        xyz ? xyz[0] : 0, xyz ? xyz[1] : 0, xyz ? xyz[2] : 0);
}
static uintptr_t oot_live_player(void) {
    uintptr_t mgr = oot_live_ptr(oot_live_main + OOT_LIVE_ACTORS);
    if (!mgr) return 0;
    void (*lock)(void *) = (void *)(oot_live_main + 0x3c42460);
    void (*unlock)(void *) = (void *)(oot_live_main + 0x3c42470);
    lock((void *)mgr);
    uintptr_t stack[96], node = oot_live_ptr(mgr + 0x38), found = 0;
    unsigned depth = 0, visited = 0;
    while ((node || depth) && visited++ < 8192 && !found) {
        while (node && depth < 96) { stack[depth++] = node; node = oot_live_ptr(node); }
        if (node || !depth) break;
        node = stack[--depth];
        uintptr_t entry = node - 8;
        for (unsigned i = 0; entry && i < 256; i++, entry = oot_live_ptr(entry + 0x38)) {
            uintptr_t actor = oot_live_ptr(entry + 0x28);
            char name[7]; uint32_t state = 0; float xyz[3];
            if (oot_live_ptr(actor) == oot_live_main + OOT_LIVE_ACTOR_VT &&
                oot_live_read(oot_live_ptr(actor + 0x20), name, sizeof name) && !memcmp(name, "Player", 7) &&
                oot_live_read(actor + 0x2c, &state, 4) && state >= 2 && state < 7 &&
                oot_live_read(actor + 0x31c, xyz, sizeof xyz) && oot_live_xyz(xyz)) { found = actor; break; }
        }
        node = oot_live_ptr(node + 8);
    }
    unlock((void *)mgr);
    return found;
}
static uintptr_t oot_live_controller(uintptr_t actor) {
    uintptr_t a = oot_live_ptr(actor + 0x270);
    a = oot_live_ptr(a + 0x50); a = oot_live_ptr(a + 0x20);
    a = oot_live_ptr(a + 0x10); a = oot_live_ptr(a + 8);
    return oot_live_ptr(a) == oot_live_main + OOT_LIVE_CCT_VT &&
        oot_live_ptr(oot_live_ptr(a + 0x20)) == oot_live_main + OOT_LIVE_BODY_VT ? a : 0;
}
typedef struct { const char *key, *banc, *spawn, *arrival; float position[3]; } OotLiveDestination;
static const OotLiveDestination oot_live_destinations[] = {
    {"field", "HyruleField", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive", {198.1793f, 163.3825f, 309.6347f}},
    {"kokiri", "KokiriForest", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive", {-126.4757f, 114.6973f, -38.3590f}},
    {"kakariko", "KakarikoVillage", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01", {-119.7726f, 96.6308f, 58.6956f}},
    {"crater", "DeathMountainCrater", "Crater_MountainTrailSide", "DeathMountainCrater_Arrive", {16.5389f, 131.8910f, 59.3945f}},
};
/* The standard exit event, using the same request constructor/submitter as SeqOneShotRequestEvent.
   All offered spawns have a real exit actor, which the arrival event needs as its starter.
   Warp-song pads intentionally aren't offered through this event. */
static int oot_live_warp(uintptr_t actor, int destination, char target[64]) {
    const OotLiveDestination *d = &oot_live_destinations[destination];
    uintptr_t mgr = oot_live_ptr(oot_live_ptr(oot_live_main + OOT_LIVE_EVENT));
    if (!mgr) return 0;
    int (*is_child)(void *) = (void *)(oot_live_main + 0x4b6db0);
    snprintf(target, 64, "%s_%s", d->banc, is_child((void *)actor) ? "Child" : "Adult");
    char current[64];
    if (oot_live_string(0x1d6189da, current) && !strcmp(current, target)) return 2;
    /* The event submitter copies parameter values before returning. */
    _Alignas(16) unsigned char request[0x1b20] = {0};
    void (*construct)(void *) = (void *)(oot_live_main + 0xd79f10);
    int (*submit)(void *, void *) = (void *)(oot_live_main + 0xd7a820);
    void (*release)(void *) = (void *)(oot_live_main + 0x20b6f0);
    construct(request);
    char *name; uint32_t cap;
    memcpy(&name, request + 0x1a90, 8); memcpy(&cap, request + 0x1a98, 4);
    int accepted = 0;
    if ((uintptr_t)name >= (uintptr_t)request && (uintptr_t)name + cap <= (uintptr_t)request + sizeof request && cap >= 23) {
        strcpy(name, "Dm_SY_BancChangeGo");
        const char *names[] = {"NextBanc", "NextStartPos", "NextStartPosAir", "BootEventName", "IsCheckAir"};
        const char *values[] = {target, d->spawn, d->spawn, d->arrival};
        for (unsigned i = 0; i < 5; i++) {
            unsigned char *entry = request + 0x18 + i * 0xc8;
            uint32_t type = i == 4 ? 4 : 1;
            memcpy(entry, &type, 4);
            /* Constructor-owned name buffer at entry+0x14; 128 bytes. */
            strcpy((char *)entry + 0x14, names[i]);
            if (i < 4) memcpy(entry + 0x98, values + i, sizeof(char *));
            else entry[0x98] = 0;
        }
        uint32_t n = 5; memcpy(request + 0x10, &n, 4);
        accepted = submit((void *)mgr, request);
    }
    uintptr_t sub_vtable = oot_live_main + 0x4974b20;
    memcpy(request + 0x1968, &sub_vtable, 8); release(request + 0x1968);
    memcpy(request + 0x1950, &sub_vtable, 8); release(request + 0x1950);
    return accepted;
}
void runtime_oot_live_init(Module *m) {
    const char *status = getenv("OOT_EDITOR_STATUS");
    if (!status || !*status || !getenv("TOTK_INPUT_FILE") || getenv("TOTK_EXPORT_IOS")) return;
    /* Post-rewrite instruction fingerprints; no optional live feature can crash an unsupported build. */
    const struct { size_t off; uint32_t word; } guards[] = {
        {0x6b254c, 0xf9401000}, {0x6b25b8, 0xf9412908},
        {0xd7a820, 0xb940b808}, {0x32db244, 0xf9404908},
        {0x4b6db4, 0xd10083ff}, {0xd79f14, 0xa9bd7bfd},
        {0x20b6f4, 0xa9bc7bfd}, {0x12dbe4, 0xfc1c0fe8}
    };
    if (strcmp(m->name, "main") || m->size < OOT_LIVE_ACTORS + 8u) return;
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) {
        uint32_t word; memcpy(&word, m->base + guards[i].off, 4);
        if (word != guards[i].word) { fprintf(stderr, "[oot-live] disabled: update fingerprint differs at %#zx\n", guards[i].off); return; }
    }
    int fd = open(status, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (fd < 0) return;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid()) { close(fd); return; }
    oot_live_fd = fd; oot_live_main = (uintptr_t)m->base;
    fprintf(stderr, "[oot-live] OOT_EDITOR_LIVE_V1 enabled\n");
    oot_live_reply("session", "ready", NULL);
}
int runtime_oot_live_command(const char *line) {
    if (strncmp(line, "oot-live ", 9)) return 0;
    char id[40] = {0}, op[24] = {0}; int used = 0;
    if (sscanf(line, "oot-live %39s %23s %n", id, op, &used) != 2) return 1;
    for (unsigned i = 0; id[i]; i++) if (!isalnum((unsigned char)id[i]) && id[i] != '-') return 1;
    if (!oot_live_main) { oot_live_reply(id, "unsupported", NULL); return 1; }
    float xyz[4] = {0}; int n = 0, kind = 0, destination = 0; char map[64] = {0}, key[24] = {0};
    if (!strcmp(op, "position") && !line[used]) kind = 1;
    else if (!strcmp(op, "move") && sscanf(line + used, "%63s %f %f %f %n", map, xyz, xyz + 1, xyz + 2, &n) == 4 && n && !line[used + n] && oot_live_xyz(xyz)) kind = 2;
    else if (!strcmp(op, "warp") && sscanf(line + used, "%23s %n", key, &n) == 1 && n && !line[used + n]) {
        for (unsigned i = 0; i < sizeof oot_live_destinations / sizeof *oot_live_destinations; i++)
            if (!strcmp(key, oot_live_destinations[i].key)) { kind = 3; destination = (int)i; }
    }
    if (!kind) { oot_live_reply(id, "invalid", NULL); return 1; }
    pthread_mutex_lock(&oot_live_lock);
    if (oot_live_request.kind) oot_live_reply(id, "busy", NULL);
    else {
        oot_live_request.kind = kind; strcpy(oot_live_request.id, id);
        oot_live_queue_deadline = runtime_now_ns() + 5000000000ULL;
        oot_live_request.destination = destination; strcpy(oot_live_request.map, map);
        memcpy(oot_live_request.xyz, xyz, sizeof xyz);
        atomic_store_explicit(&oot_live_pending, 1, memory_order_release);
    }
    pthread_mutex_unlock(&oot_live_lock);
    return 1;
}
/* Called after releasing inputLock, on the guest's controller-poll thread. */
void runtime_oot_live_tick(void) {
    if (!atomic_load_explicit(&oot_live_pending, memory_order_acquire)) return;
    pthread_mutex_lock(&oot_live_lock);
    int kind = oot_live_request.kind; char id[40]; float xyz[4];
    strcpy(id, oot_live_request.id); memcpy(xyz, oot_live_request.xyz, sizeof xyz);
    if (kind <= 3 && runtime_now_ns() > oot_live_queue_deadline) { oot_live_reply(id, "expired", NULL); goto done; }
    uint64_t now = runtime_now_ns();
    if (kind == 4 && now < oot_live_next_check) goto waiting;
    if (kind == 4) oot_live_next_check = now + 100000000ULL;
    uintptr_t actor = oot_live_player(); float current[3];
    char map[64] = {0}; int has_map = oot_live_string(0x1d6189da, map);
    if (kind == 4) {
        int ready = actor && has_map && !strcmp(map, oot_live_check_map) && !oot_live_paused() &&
            oot_live_controller(actor) && oot_live_read(actor + 0x31c, current, sizeof current);
        float d2 = 0;
        if (ready) for (int i = 0; i < 3; i++) {
            float d = current[i] - oot_live_destinations[oot_live_request.destination].position[i]; d2 += d*d;
        }
        if (!ready || d2 > 144) oot_live_ready_since = 0;
        else {
            if (!oot_live_ready_since) oot_live_ready_since = now;
            if (now - oot_live_ready_since >= 750000000ULL && now - oot_live_started >= 3000000000ULL) {
                oot_live_reply(id, "arrived", current); goto done;
            }
        }
        if (runtime_now_ns() >= oot_live_deadline) { oot_live_reply(id, "warp-timeout", NULL); goto done; }
        goto waiting;
    }
    if (!actor || !oot_live_read(actor + 0x31c, current, sizeof current)) { oot_live_reply(id, "not-ready", NULL); goto done; }
    if (kind == 1) { oot_live_reply(id, "position", current); goto done; }
    if (kind == 5) {
        if (--oot_live_check_frames > 0) goto waiting;
        float d2 = 0; for (int i = 0; i < 3; i++) { float d = current[i] - oot_live_check_xyz[i]; d2 += d*d; }
        oot_live_reply(id, has_map && !strcmp(map, oot_live_check_map) && d2 < 4 ? "moved" : "move-failed", current);
        goto done;
    }
    if (!has_map || oot_live_paused() || runtime_fs_save_busy()) { oot_live_reply(id, "busy", current); goto done; }
    if (kind == 3) {
        int result = oot_live_warp(actor, oot_live_request.destination, oot_live_check_map);
        if (result == 2) { oot_live_reply(id, "already-here", current); goto done; }
        if (!result) { oot_live_reply(id, "warp-rejected", current); goto done; }
        oot_live_ready_since = 0; oot_live_next_check = 0; oot_live_started = runtime_now_ns();
        oot_live_request.kind = 4; oot_live_deadline = runtime_now_ns() + 180000000000ULL;
        oot_live_reply(id, "warping", current); goto waiting;
    }
    uintptr_t controller = oot_live_controller(actor);
    float d2 = 0; for (int i = 0; i < 3; i++) { float d = xyz[i] - current[i]; d2 += d * d; }
    if (strcmp(map, oot_live_request.map)) { oot_live_reply(id, "wrong-area", current); goto done; }
    if (!controller) { oot_live_reply(id, "busy", current); goto done; }
    if (d2 > 250 * 250) { oot_live_reply(id, "too-far", current); goto done; }
    void (*change)(void *, const float *) = (void *)(oot_live_main + 0x6b2530);
    void (*velocity)(void *, const float *, int) = (void *)(oot_live_main + 0x12dbe0);
    float zero[4] = {0}; velocity((void *)oot_live_ptr(controller + 0x20), zero, 0);
    change((void *)controller, xyz);
    memcpy(oot_live_check_xyz, xyz, sizeof oot_live_check_xyz); strcpy(oot_live_check_map, map);
    oot_live_check_frames = 3; oot_live_request.kind = 5;
    goto waiting;
done:
    oot_live_request.kind = 0; atomic_store(&oot_live_pending, 0);
waiting:
    pthread_mutex_unlock(&oot_live_lock);
}
