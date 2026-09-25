// Wasteland Storms -- storm frequency presets for Mad Max (2015, PC).
//
// Standalone ASI plugin, split out of the Enhanced Convoys / MadMaxEzStorm
// dev build on 2026-09-23 so the two mods install and run independently.
// Depends on MinHook only; every game address is found at startup by byte
// signature (patterns taken from the GOG 2015-12-07 build, see gen_sigs.py
// in the research notes), so nothing here is tied to one executable.
//
// How it works, in short:
//  - The game's own storm command, NEvent::CSendEvent::SendMsg("storm.trigger"),
//    raises a real storm (sandstorm or thunderstorm, the game picks).
//  - The weather manager (CEnvironmentPresetTimeOfDayManager) says which
//    storm-class preset is active; that is how the mod knows a storm has
//    reached the player, when it ends, and when the player is inside a local
//    atmosphere (Gastown, sulfur pits) where storms cannot start.
//  - A small scheduler counts the gap from the real end of a storm, in game
//    time, under clear sky only.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include "MinHook.h"

// 1 while developing (F11 = one storm now, extra log lines). Release: 0.
#define WS_DEV 0
#define WS_VERSION "0.9.1-beta2"

// ---------------------------------------------------------------- logging --
static char g_modDir[MAX_PATH] = "";
static char g_logPath[MAX_PATH] = "WastelandStorms.log";
static char g_iniPath[MAX_PATH] = "WastelandStorms.ini";
static DWORD g_logStart = 0;

static void InitPaths(HMODULE self) {
    if (GetModuleFileNameA(self, g_modDir, MAX_PATH)) {
        char* slash = strrchr(g_modDir, '\\');
        if (slash) slash[1] = 0;
        snprintf(g_logPath, MAX_PATH, "%sWastelandStorms.log", g_modDir);
        snprintf(g_iniPath, MAX_PATH, "%sWastelandStorms.ini", g_modDir);
    }
}

void LogLine(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD t = GetTickCount() - g_logStart;
    fprintf(f, "[%02d:%02d:%02d +%5lu.%01lus] ", st.wHour, st.wMinute, st.wSecond, (unsigned long)(t / 1000), (unsigned long)((t % 1000) / 100));
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// The previous session's log is kept as WastelandStorms.previous.log: a quick
// relaunch of the game used to wipe the log of the session that mattered.
static void LogStart() {
    g_logStart = GetTickCount();
    char prev[MAX_PATH];
    snprintf(prev, MAX_PATH, "%sWastelandStorms.previous.log", g_modDir);
    MoveFileExA(g_logPath, prev, MOVEFILE_REPLACE_EXISTING);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
}

// ------------------------------------------------------ signature scanning --
struct GameSignature {
    const char* name;
    const char* pattern;   // "48 8B ?? ..." -- ?? is a wildcard byte
    bool required;
    uintptr_t resolved;
    int matches;
    bool alreadyHooked;     // found behind another mod's hook (E9 jmp at the start)
};
static GameSignature g_sigs[] = {
    { "CPlayer::UpdateController", "48 8B C4 41 54 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 0F 29 70 E8 0F 28 F1 48 8B E9 F3 0F 10 81 00 03 00 00", true, 0, 0, false },
    { "NEvent::CSendEvent::SendMsg", "48 81 EC 68 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 02 00 00 48 8D 54 24 50", true, 0, 0, false },
    { "CGameObject::FindOptional", "48 8B C4 57 48 83 EC 70 48 C7 44 24 48 FE FF FF FF 48 89 58 08 48 89 70 10", true, 0, 0, false },
    { "CEnvironmentPresetTimeOfDayManager::InternalUpdateRender", "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 48 81 EC 80 00 00 00", false, 0, 0, false },
    { "GameGuiMessageQueuePush", "48 89 74 24 10 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 0F B6 F2 48 8B F9 48 85 C0", false, 0, 0, false },
    { "CLocator::GetLocalizedStringRaw", "40 53 48 83 EC 30 4C 8B 41 28 8B DA 49 8B 40 08 80 78 49 00 75 ?? 39 50 18", false, 0, 0, false },
    { "CNodeActivationTagCallback::Register (environment manager pointer)", "F3 0F 11 11 4C 8B C1 48 8B 0D ?? ?? ?? ?? 44 8B CA 48 8D 15 ?? ?? ?? ??", false, 0, 0, false },
};
enum { SIG_UPDATECONTROLLER = 0, SIG_SENDMSG, SIG_FINDOPTIONAL, SIG_TOD_RENDER, SIG_GUIPUSH, SIG_LOCRAW, SIG_TAGREGISTER, SIG_COUNT };

static uintptr_t g_textStart = 0, g_textEnd = 0;
static unsigned int g_exeTimeStamp = 0;
static char g_versionStatus[240] = "not checked";

static bool ScanBytes(const unsigned char* bytes, const bool* wild, int len, GameSignature& sig) {
    sig.matches = 0; sig.resolved = 0;
    const unsigned char* p = (const unsigned char*)g_textStart;
    const unsigned char* end = (const unsigned char*)g_textEnd - len;
    for (; p <= end; p++) {
        if (!wild[0] && p[0] != bytes[0]) continue;
        int i = 1;
        for (; i < len; i++) if (!wild[i] && p[i] != bytes[i]) break;
        if (i == len) {
            if (++sig.matches == 1) sig.resolved = (uintptr_t)p;
            else break;
        }
    }
    return sig.matches == 1;
}

// Another ASI may already have hooked the same function (Enhanced Convoys and
// Wasteland Storms both hook CPlayer::UpdateController). MinHook replaces the
// first 5 bytes with `jmp rel32` (E9 xx xx xx xx) and leaves every byte after
// them untouched, so if the plain pattern finds nothing, look for exactly
// that: E9 + 4 wildcards + the original pattern from byte 5 on.
static bool ScanPattern(GameSignature& sig) {
    unsigned char bytes[128]; bool wild[128]; int len = 0;
    for (const char* c = sig.pattern; *c && len < 128; ) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') { wild[len] = true; bytes[len] = 0; len++; c += 2; continue; }
        unsigned int v = 0; sscanf_s(c, "%2x", &v); bytes[len] = (unsigned char)v; wild[len] = false; len++; c += 2;
    }
    sig.alreadyHooked = false;
    if (ScanBytes(bytes, wild, len, sig)) return true;
    if (sig.matches != 0 || len < 12) return false;
    bytes[0] = 0xE9; wild[0] = false;
    for (int i = 1; i < 5; i++) wild[i] = true;
    if (!ScanBytes(bytes, wild, len, sig)) return false;
    sig.alreadyHooked = true;
    return true;
}

static bool ResolveGameAddresses() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { snprintf(g_versionStatus, sizeof(g_versionStatus), "no DOS header"); return false; }
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { snprintf(g_versionStatus, sizeof(g_versionStatus), "no PE header"); return false; }
    g_exeTimeStamp = nt->FileHeader.TimeDateStamp;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            g_textStart = base + sec->VirtualAddress;
            g_textEnd = g_textStart + sec->Misc.VirtualSize;
            break;
        }
    }
    if (!g_textStart) { snprintf(g_versionStatus, sizeof(g_versionStatus), "no .text section"); return false; }

    int failed = 0; char failList[200] = "";
    for (int i = 0; i < SIG_COUNT; i++) {
        if (!ScanPattern(g_sigs[i]) && g_sigs[i].required) {
            failed++;
            size_t l = strlen(failList);
            snprintf(failList + l, sizeof(failList) - l, "%s%s(x%d)", l ? ", " : "", g_sigs[i].name, g_sigs[i].matches);
        }
    }
    if (failed) {
        snprintf(g_versionStatus, sizeof(g_versionStatus), "signature not found: %s (exe timestamp 0x%08X)", failList, g_exeTimeStamp);
        return false;
    }
    snprintf(g_versionStatus, sizeof(g_versionStatus), "OK (exe timestamp 0x%08X, %s)", g_exeTimeStamp,
        g_exeTimeStamp == 0x566520B2u ? "the GOG build this was made on" : "a build not seen by the author -- please report how it works");
    return true;
}

// ------------------------------------------------------------ game access --
typedef void (*SendMsgFn)(const char* msg);
typedef bool (*FindOptionalFn)(uint64_t id, void* outSharedPtr);
static SendMsgFn SendEventMsg = nullptr;
static FindOptionalFn CGameObject_FindOptional = nullptr;

// CGameObject::FindOptional hands back a boost::shared_ptr {px, pn}; release
// the reference it added (sp_counted_base: vptr, use_count_, weak_count_).
// The object stays owned by the world, so this never frees it.
static void* FindGameObjectByIdAndRelease(uint64_t id) {
    uint8_t sp[16] = { 0 };
    if (!CGameObject_FindOptional || !CGameObject_FindOptional(id, sp)) return nullptr;
    void* px = *(void**)sp;
    void* pn = *(void**)(sp + 8);
    if (pn) InterlockedDecrement((volatile long*)((uintptr_t)pn + 8));
    return px;
}

// ------------------------------------------------------- environment tags --
// Places where the game never lets a storm happen (strongholds, interiors,
// camps, tunnels, Griffa spots...) are CEnvironmentPresetLocal volumes that
// raise the tag "no_storm" -- 240 of them in the data. The environment
// manager is (*global) + 0x78, the pointer CNodeActivationTagCallback::
// Register loads with `mov rcx,[rip+disp32]` at +7.
static void** g_envManagerGlobal = nullptr;
static uint32_t g_tagNoStorm = 0;

// Jenkins lookup3 hashlittle, seed 0 -- the engine's string hash.
uint32_t WsJenkinsHash(const char* str);
static uint32_t JenkinsHash(const char* str) { return WsJenkinsHash(str); }
uint32_t WsJenkinsHash(const char* str) {
    const uint8_t* k = (const uint8_t*)str; uint32_t length = (uint32_t)strlen(str);
    uint32_t a, b, c; a = b = c = 0xDEADBEEF + length;
    #define ROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
    while (length > 12) {
        a += k[0] | (k[1] << 8) | (k[2] << 16) | ((uint32_t)k[3] << 24);
        b += k[4] | (k[5] << 8) | (k[6] << 16) | ((uint32_t)k[7] << 24);
        c += k[8] | (k[9] << 8) | (k[10] << 16) | ((uint32_t)k[11] << 24);
        a -= c; a ^= ROT(c, 4); c += b;  b -= a; b ^= ROT(a, 6);  a += c;
        c -= b; c ^= ROT(b, 8); b += a;  a -= c; a ^= ROT(c, 16); c += b;
        b -= a; b ^= ROT(a, 19); a += c; c -= b; c ^= ROT(b, 4);  b += a;
        length -= 12; k += 12;
    }
    switch (length) {
    case 12: c += (uint32_t)k[11] << 24; [[fallthrough]];
    case 11: c += k[10] << 16; [[fallthrough]];
    case 10: c += k[9] << 8; [[fallthrough]];
    case 9:  c += k[8]; [[fallthrough]];
    case 8:  b += (uint32_t)k[7] << 24; [[fallthrough]];
    case 7:  b += k[6] << 16; [[fallthrough]];
    case 6:  b += k[5] << 8; [[fallthrough]];
    case 5:  b += k[4]; [[fallthrough]];
    case 4:  a += (uint32_t)k[3] << 24; [[fallthrough]];
    case 3:  a += k[2] << 16; [[fallthrough]];
    case 2:  a += k[1] << 8; [[fallthrough]];
    case 1:  a += k[0]; break;
    case 0:  return c;
    }
    c ^= b; c -= ROT(b, 14); a ^= c; a -= ROT(c, 11); b ^= a; b -= ROT(a, 25);
    c ^= b; c -= ROT(b, 16); a ^= c; a -= ROT(c, 4);  b ^= a; b -= ROT(a, 14);
    c ^= b; c -= ROT(b, 24);
    #undef ROT
    return c;
}

// The tag system only tracks tags somebody subscribed to: GetTagWeight on
// "no_storm" read 0.00 even inside a stronghold (0.4/0.5 tests), and the
// table it searches turned out to hold one entry per active environment
// LAYER (+0x7C = 1 for the global layer, 0x14 for each local volume Max is
// inside; 4 local ones in a stronghold, none in the open), with no tag hash.
// So the mod subscribes the same way the game's own storm graph does:
// CNodeActivationTagCallback::Register ends in
//   jmp CEnvironmentPresetManager::RegisterActivationTagsCallback(mgr, cb, user, tag)
// and the callback receives the tag's weight whenever it changes.
typedef void (*TagCallbackFn)(float weight, uint32_t tag, void* user);
typedef void (*RegisterTagCallbackFn)(void* mgr, TagCallbackFn cb, void* user, uint32_t tag);
static RegisterTagCallbackFn RegisterTagCallback = nullptr;
static void* g_tagSubscribedMgr = nullptr;
static volatile float g_noStormWeight = -1.0f;   // -1 until the game has reported it
static int g_tagCallbacks = 0;

static void NoStormTagCallback(float weight, uint32_t tag, void*) {
    g_noStormWeight = weight;
    g_tagCallbacks++;
}

// Called each second once the world exists; (re)subscribes when the manager
// is new (first load, or a reload that rebuilt it).
static void EnsureTagSubscription() {
    if (!RegisterTagCallback || !g_envManagerGlobal) return;
    __try {
        void* base = *g_envManagerGlobal;
        if (!base) return;
        void* mgr = (uint8_t*)base + 0x78;
        if (mgr == g_tagSubscribedMgr) return;
        RegisterTagCallback(mgr, NoStormTagCallback, nullptr, g_tagNoStorm);
        g_tagSubscribedMgr = mgr;
        LogLine("environment tags: subscribed to no_storm");
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("environment tags: subscription faulted"); }
}

static int LocalLayerCount();

// True while Max stands where the game won't raise a storm: no_storm as the
// game reports it once subscribed; before the first report, "inside any
// local environment volume" stands in for it.
static bool InNoStormArea() {
    float w = g_noStormWeight;
    if (w >= 0.0f) return w >= 0.5f;
    return LocalLayerCount() > 0;
}

// Local environment layers Max is inside right now (strongholds, interiors,
// camps, caves...). -1 when unreadable.
static int LocalLayerCount() {
    if (!g_envManagerGlobal) return -1;
    __try {
        uint8_t* base = (uint8_t*)*g_envManagerGlobal;
        if (!base) return -1;
        uint8_t* mgr = base + 0x78;
        uint8_t* it = *(uint8_t**)(mgr + 0x15FA0);
        uint8_t* end = *(uint8_t**)(mgr + 0x15FA8);
        if (!it || end < it) return -1;
        int n = 0;
        for (; it + 0x84 <= end; it += 0x84) if (*(uint32_t*)(it + 0x7C) == 0x14) n++;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}


// ---------------------------------------------------------- weather signal --
// CEnvironmentPresetTimeOfDayManager keeps the active storm-class preset at
//     int index = *(int*)(mgr + 0x270)          (-1 = none)
//     state     = mgr + 0x278 + index * 0x48     (SSandstormState)
// and the state's +0x3C holds the preset's weather id
// (global/environment_presets.blo):
//     -1 Sandstorm, -2 ThunderStorm    real storms
//     -3 SulfurStorm, -4 Gastown       local atmospheres holding the same
//                                      slot; no storm can start while on
// Measured live 2026-09-23: a storm reaches a waiting player 40-55 s after
// the trigger and lasts 330 s (300 s + 30 s fade); a player driving away is
// never reached.
// The manager is captured from its own per-frame InternalUpdateRender (exact
// `this`) when that signature resolves; otherwise from the world lookup of
// its objectid, which returns the manager + 0x10 (secondary base, confirmed
// live), accepted only if the index reads as a sane -1..7.
static const uint64_t TIME_OF_DAY_MANAGER_OBJECT_ID = 4080819380ull;
static const int STORM_INDEX_OFF = 0x270, STORM_STATE_BASE = 0x278, STORM_STATE_STRIDE = 0x48, STORM_SLOTS = 8;

static void* volatile g_timeOfDayMgr = nullptr;
static void (*TodInternalUpdateRender_orig)(void* self, float dt) = nullptr;
static void TodInternalUpdateRender_hook(void* self, float dt) {
    g_timeOfDayMgr = self;
    TodInternalUpdateRender_orig(self, dt);
}

enum WeatherKind { WEATHER_UNKNOWN = 0, WEATHER_CLEAR, WEATHER_STORM, WEATHER_LOCAL };
struct WeatherReading { bool ok; int index; int weatherId; float timer; };
static WeatherReading g_weather = { false, -1, 0, 0.0f };
static WeatherKind g_weatherKind = WEATHER_UNKNOWN;
static float g_weatherClock = 0.0f, g_gameTime = 0.0f, g_stormStartedAt = -1.0f;
static float g_worldReadyAt = -1.0f;   // game time when the weather manager first answered

static const char* WeatherName(int id) {
    switch (id) {
    case -1: return "Sandstorm";
    case -2: return "ThunderStorm";
    case -3: return "SulfurStorm (local)";
    case -4: return "Gastown (local)";
    }
    return "unknown";
}

static bool ReadWeather(void* mgr, WeatherReading* out) {
    __try {
        int idx = *(int*)((uintptr_t)mgr + STORM_INDEX_OFF);
        if (idx < -1 || idx >= STORM_SLOTS) return false;
        out->index = idx; out->weatherId = 0; out->timer = 0.0f;
        if (idx >= 0) {
            uintptr_t st = (uintptr_t)mgr + STORM_STATE_BASE + idx * STORM_STATE_STRIDE;
            out->timer = *(float*)(st + 0x38);
            out->weatherId = *(int*)(st + 0x3C);
        }
        out->ok = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* FindWeatherManager() {
    if (g_timeOfDayMgr) return g_timeOfDayMgr;
    void* byId = FindGameObjectByIdAndRelease(TIME_OF_DAY_MANAGER_OBJECT_ID);
    return byId ? (void*)((uintptr_t)byId - 0x10) : nullptr;
}

static void OnWeatherChanged(const WeatherReading& prev, WeatherKind prevKind, const WeatherReading& now, WeatherKind kind) {
    if (kind == WEATHER_STORM && prevKind != WEATHER_STORM) {
        g_stormStartedAt = g_gameTime;
        LogLine("storm arrived: %s", WeatherName(now.weatherId));
    } else if (prevKind == WEATHER_STORM && kind != WEATHER_STORM) {
        LogLine("storm ended: %s after %.0f s", WeatherName(prev.weatherId), g_stormStartedAt >= 0.0f ? g_gameTime - g_stormStartedAt : -1.0f);
        g_stormStartedAt = -1.0f;
    }
    if (kind == WEATHER_LOCAL && prevKind != WEATHER_LOCAL)
        LogLine("entered local atmosphere %s (storms wait until you leave)", WeatherName(now.weatherId));
    else if (prevKind == WEATHER_LOCAL && kind != WEATHER_LOCAL)
        LogLine("left local atmosphere %s", WeatherName(prev.weatherId));
}

static void WeatherTick(float dt) {
    g_gameTime += dt;
    g_weatherClock += dt;
    if (g_weatherClock < 1.0f) return;
    g_weatherClock = 0.0f;
    void* mgr = FindWeatherManager();
    WeatherReading r = { false, -1, 0, 0.0f };
    if (!mgr || !ReadWeather(mgr, &r)) {
        if (g_weather.ok) LogLine("weather signal lost");
        g_weather.ok = false; g_weatherKind = WEATHER_UNKNOWN;
        return;
    }
    EnsureTagSubscription();
    WeatherKind kind = r.index < 0 ? WEATHER_CLEAR : (r.weatherId == -1 || r.weatherId == -2) ? WEATHER_STORM : WEATHER_LOCAL;
    if (!g_weather.ok) {
        LogLine("weather signal acquired (%s): %s", g_timeOfDayMgr ? "render hook" : "object lookup",
            kind == WEATHER_CLEAR ? "clear sky" : WeatherName(r.weatherId));
        if (kind == WEATHER_STORM) g_stormStartedAt = g_gameTime;
        if (g_worldReadyAt < 0.0f) g_worldReadyAt = g_gameTime;
    } else if (kind != g_weatherKind || r.index != g_weather.index) {
        OnWeatherChanged(g_weather, g_weatherKind, r, kind);
    }
    g_weather = r; g_weatherKind = kind;
}

// --------------------------------------------------------------- scheduler --
enum StormPreset { STORM_OFF = 0, STORM_FREQUENT, STORM_UNCOMMON, STORM_RANDOM, STORM_PRESET_COUNT };
struct StormPresetInfo { const char* name; float minSeconds; float maxSeconds; const char* key; const char* detail; };
static const StormPresetInfo g_presets[STORM_PRESET_COUNT] = {
    { "Off (vanilla storms only)",                        0.0f,    0.0f, "Off",      "Vanilla: the game's own storms only" },
    { "Frequent (4-9 min of clear sky between storms)", 240.0f,  540.0f, "Frequent", "4-9 minutes of clear sky between storms" },
    { "Uncommon (10-30 min of clear sky between storms)", 600.0f, 1800.0f, "Uncommon", "10-30 minutes of clear sky between storms" },
    { "Random (0-60 min, can be back to back)",           0.0f, 3600.0f, "Random",   "0-60 minutes, storms can come back to back" },
};
// Used only without a weather signal: a storm's full measured length.
static const float STORM_MEASURED_TOTAL_S = 370.0f;
// A storm that has not reached the player after this long counts as missed
// (outrun, or a place where storms cannot happen). The next one is scheduled
// normally -- never re-fired at once, so storm fronts don't pile up.
static const float STORM_ARRIVAL_TIMEOUT_S = 120.0f;

enum SchedState { SCHED_GAP = 0, SCHED_AWAITING_ARRIVAL, SCHED_IN_STORM };
static int g_preset = STORM_OFF;
static SchedState g_sched = SCHED_GAP;
static float g_nextDelay = 0.0f, g_awaitingFor = 0.0f;
static int g_triggers = 0, g_arrived = 0, g_missed = 0;

static float RollDelay() {
    const StormPresetInfo& p = g_presets[g_preset];
    float t = (float)rand() / (float)RAND_MAX;
    return p.minSeconds + t * (p.maxSeconds - p.minSeconds);
}

static void TriggerStorm(const char* why) {
    if (!SendEventMsg) return;
    g_triggers++;
    SendEventMsg("storm.trigger");
    LogLine("storm triggered (%s, #%d)", why, g_triggers);
}

static void StartGap(const char* why) {
    g_sched = SCHED_GAP;
    g_nextDelay = RollDelay();
    LogLine("%s; next storm after %.0f s of clear sky", why, g_nextDelay);
}

static void SchedulerTick(float dt) {
    if (g_preset == STORM_OFF) return;
    bool haveSignal = g_weather.ok;
    bool storm = haveSignal && g_weatherKind == WEATHER_STORM;
    bool local = haveSignal && g_weatherKind == WEATHER_LOCAL;

    if (!haveSignal) {
        g_nextDelay -= dt;
        if (g_nextDelay > 0.0f) return;
        TriggerStorm("timed mode, no weather signal");
        g_nextDelay = RollDelay() + STORM_MEASURED_TOTAL_S;
        return;
    }

    switch (g_sched) {
    case SCHED_GAP:
        if (storm) {
            g_sched = SCHED_IN_STORM;
            LogLine("a storm is on while waiting; the gap restarts when it ends");
            return;
        }
        // Hold while Max is somewhere a storm can't reach him -- Gastown and
        // the sulfur pits (their own atmosphere holds the storm slot), or a
        // stronghold, interior or camp (no_storm). The storm comes after he
        // walks out instead of being wasted.
        {
            static int lastHold = 0;
            int hold = local ? 1 : (InNoStormArea() ? 2 : 0);
            if (hold != lastHold) {
                if (hold == 1) LogLine("next storm on hold: inside Gastown / the sulfur pits");
                else if (hold == 2) LogLine("next storm on hold: inside a stronghold, interior or camp");
                else LogLine("next storm countdown resumed (%.0f s left)", g_nextDelay);
                lastHold = hold;
            }
            if (hold) return;
        }
        g_nextDelay -= dt;
        if (g_nextDelay > 0.0f) return;
        TriggerStorm(g_presets[g_preset].name);
        g_sched = SCHED_AWAITING_ARRIVAL;
        g_awaitingFor = 0.0f;
        return;

    case SCHED_AWAITING_ARRIVAL:
        if (storm) {
            g_arrived++;
            g_sched = SCHED_IN_STORM;
            LogLine("storm #%d reached the player after %.0f s", g_triggers, g_awaitingFor);
            return;
        }
        g_awaitingFor += dt;
        if (g_awaitingFor >= STORM_ARRIVAL_TIMEOUT_S) {
            g_missed++;
            char why[160];
            snprintf(why, sizeof(why), "storm #%d never reached the player in %.0f s (%s)", g_triggers, STORM_ARRIVAL_TIMEOUT_S,
                local ? "inside a local atmosphere" : "outrun, or a no-storm area");
            StartGap(why);
        }
        return;

    case SCHED_IN_STORM:
        if (!storm) StartGap("storm over");
        return;
    }
}

// ------------------------------------------------------------- settings --
// WastelandStorms.ini, next to the .asi. Written whenever the preset changes.
void Notice_Init();
void Notice_Show(const char* title, const char* detail, int milliseconds);

static int g_hotkey = VK_F5;
static bool g_startupNotice = true;

static void SaveSettings() {
    WritePrivateProfileStringA("WastelandStorms", "Preset", g_presets[g_preset].key, g_iniPath);
}

static void LoadSettings() {
    char buf[64];
    GetPrivateProfileStringA("WastelandStorms", "Preset", "Off", buf, sizeof(buf), g_iniPath);
    for (int i = 0; i < STORM_PRESET_COUNT; i++)
        if (_stricmp(buf, g_presets[i].key) == 0) g_preset = i;
    GetPrivateProfileStringA("WastelandStorms", "Hotkey", "0x74", buf, sizeof(buf), g_iniPath);
    int key = (int)strtol(buf, nullptr, 0);
    if (key > 0 && key < 256) g_hotkey = key;
    g_startupNotice = GetPrivateProfileIntA("WastelandStorms", "StartupNotice", 1, g_iniPath) != 0;
    // write a complete, commented file the first time
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (fopen_s(&f, g_iniPath, "w") == 0 && f) {
            fprintf(f,
                "; Wasteland Storms settings. The preset is also changed in game with the hotkey.\n"
                "[WastelandStorms]\n"
                "; Off, Frequent, Uncommon or Random\n"
                "Preset=%s\n"
                "; Key that cycles the presets, as a Windows virtual-key code (0x74 = F5, 0x75 = F6, ...)\n"
                "Hotkey=0x%02X\n"
                "; Show the current preset on screen when the game starts (1 = yes, 0 = no)\n"
                "StartupNotice=%d\n", g_presets[g_preset].key, g_hotkey, g_startupNotice ? 1 : 0);
            fclose(f);
        }
    }
    LogLine("settings: preset %s, hotkey 0x%02X, startup notice %s", g_presets[g_preset].key, g_hotkey, g_startupNotice ? "on" : "off");
}

// The game's own storm-alert banner when the HUD hooks are in; the overlay
// box otherwise (e.g. a build where those two signatures are not found).
// Notices are drawn by the mod itself (notice.cpp) with the game's own UI
// font. Pushing them into the game's HUD queue was tried (0.3/0.4): the only
// banner that fits is the storm alert, which plays its own sound and effect,
// and a banner of ours of that type also kept later real storm alerts off
// the screen. So the game's HUD is left completely alone.
static void ShowNotice(const char* title, const char* detail, float seconds) {
    Notice_Show(title, detail, (int)(seconds * 1000.0f));
}

static void ShowPresetNotice(int ms) {
    char title[96];
    snprintf(title, sizeof(title), "Wasteland Storms: %s", g_presets[g_preset].key);
    ShowNotice(title, g_presets[g_preset].detail, ms / 1000.0f);
}

static void CyclePreset() {
    g_preset = (g_preset + 1) % STORM_PRESET_COUNT;
    SaveSettings();
    ShowPresetNotice(4000);
    if (g_preset == STORM_OFF) { LogLine("preset: %s", g_presets[g_preset].name); return; }
    char why[160];
    snprintf(why, sizeof(why), "preset: %s", g_presets[g_preset].name);
    if (g_weather.ok && g_weatherKind == WEATHER_STORM) {
        g_sched = SCHED_IN_STORM;
        LogLine("%s (a storm is on; the gap starts when it ends)", why);
    } else {
        StartGap(why);
    }
}

// -------------------------------------------------------------- frame hook --
static bool g_f5Down = false;
#if WS_DEV
static bool g_f11Down = false, g_f12Down = false;
#endif

static void (*UpdateController_orig)(void* thiz, float dt) = nullptr;
static bool g_firstTickDone = false;

static bool GameHasFocus() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void UpdateController_hook(void* thiz, float dt) {
    if (!g_firstTickDone) {
        g_firstTickDone = true;
        Notice_Init();
        if (g_preset != STORM_OFF) StartGap("loaded with the saved preset");
    }

    WeatherTick(dt);
    SchedulerTick(dt);

    // Startup notice. CPlayer::UpdateController already ticks in the menus
    // (seen live: 7 s after launch) but the weather manager only answers once
    // the open world has loaded (~2 min later). On top of that the notice
    // waits until Max stands somewhere a storm can actually happen: not in a
    // local atmosphere (Gastown, sulfur pits) and not in a "no_storm" area
    // (strongholds, interiors, camps). Loading a save inside a stronghold
    // therefore shows it when he walks out.
    static bool startupShown = false, startupWaitLogged = false;
    if (!startupShown && g_startupNotice && g_worldReadyAt >= 0.0f && g_gameTime - g_worldReadyAt >= 5.0f) {
        bool stormable = g_weatherKind != WEATHER_LOCAL && !InNoStormArea();
        if (stormable) {
            startupShown = true;
            char keyName[32] = "F5";
            UINT scan = MapVirtualKeyA(g_hotkey, MAPVK_VK_TO_VSC);
            if (scan) GetKeyNameTextA((LONG)(scan << 16), keyName, sizeof(keyName));
            char title[96], detail[96];
            snprintf(title, sizeof(title), "Wasteland Storms: %s", g_presets[g_preset].key);
            snprintf(detail, sizeof(detail), "Press %s to change how often storms come", keyName);
            ShowNotice(title, detail, 5.0f);
            LogLine("startup notice shown");
        } else if (!startupWaitLogged) {
            startupWaitLogged = true;
            LogLine("startup notice waiting: %s", g_weatherKind == WEATHER_LOCAL ? "inside Gastown / the sulfur pits" : "inside a stronghold, interior or camp");
        }
    }

    bool focus = GameHasFocus();
    bool f5 = focus && (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
    if (f5 && !g_f5Down) CyclePreset();
    g_f5Down = f5;
#if WS_DEV
    bool f11 = focus && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11 && !g_f11Down) TriggerStorm("manual F11");
    g_f11Down = f11;
    // F12: log where Max stands (tag + weather), for testing the startup rule
    bool f12 = focus && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12 && !g_f12Down) {
        LogLine("F12 probe: weather %s | no_storm (subscribed) %.2f after %d callbacks | local layers %d",
            g_weatherKind == WEATHER_CLEAR ? "clear" : g_weatherKind == WEATHER_STORM ? "storm" : g_weatherKind == WEATHER_LOCAL ? "local atmosphere" : "unknown",
            g_noStormWeight, g_tagCallbacks, LocalLayerCount());
    }
    g_f12Down = f12;
#endif

    UpdateController_orig(thiz, dt);
}

static const char* InstallHook(uintptr_t target, LPVOID hook, LPVOID* orig) {
    if (!target) return "not found";
    MH_STATUS c = MH_CreateHook((LPVOID)target, hook, orig);
    if (c != MH_OK) return MH_StatusToString(c);
    return MH_StatusToString(MH_EnableHook((LPVOID)target));
}

// ------------------------------------------------------------- startup --
// Test switch: MADMAX_MODS_DEFER=1 in the environment, or a file
// scriptsorce_steam_path.txt, forces the deferred (Steam) startup path on a
// build whose code is readable at load time.
static bool ForceDefer() {
    char v[8];
    if (GetEnvironmentVariableA("MADMAX_MODS_DEFER", v, sizeof(v)) > 0 && v[0] == '1') return true;
    char path[MAX_PATH];                             // or a marker file: <game>\scriptsorce_steam_path.txt
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\'); if (slash) slash[1] = 0;
    strcat_s(path, "scripts\\force_steam_path.txt");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void InstallMod(bool ok);
static volatile LONG g_installed = 0;
typedef void (WINAPI* GetStartupInfoWFn)(LPSTARTUPINFOW info, uintptr_t chain);
static GetStartupInfoWFn GetStartupInfoW_orig = nullptr;

// Not one signature matched: the code is not readable yet (Steam's DRM keeps
// it encrypted until the game starts), as opposed to a build that differs.
static bool NoSignatureMatched() {
    for (int i = 0; i < SIG_COUNT; i++) if (g_sigs[i].matches) return false;
    return true;
}

// The second argument is not part of GetStartupInfoW; mm_sdk-based plugins
// pass a marker through it to each other, so it is forwarded untouched.
static void WINAPI GetStartupInfoW_hook(LPSTARTUPINFOW info, uintptr_t chain) {
    GetStartupInfoW_orig(info, chain);
    if (g_installed) return;
    bool ok = ResolveGameAddresses();
    if (!ok && NoSignatureMatched()) return;           // still encrypted, wait for the next call
    if (InterlockedExchange(&g_installed, 1) == 0) {
        LogLine("game code ready (C runtime startup), installing");
        InstallMod(ok);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        LogLine("session end: %d storm(s) triggered, %d reached the player, %d missed", g_triggers, g_arrived, g_missed);
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(hModule);
    InitPaths(hModule);
    LogStart();
    srand(GetTickCount());
    // Another ASI (e.g. Enhanced Convoys) may already have initialised
    // MinHook in its own copy; each DLL has its own MinHook state, so this is
    // independent of it.
    MH_STATUS init = MH_Initialize();
    bool ok = ResolveGameAddresses();
    LogLine("Wasteland Storms %s", WS_VERSION);
    LogLine("MinHook: %s", MH_StatusToString(init));
    if ((!ok && NoSignatureMatched()) || ForceDefer()) {
        // Steam: the executable is wrapped by Steam's DRM and its code is still
        // encrypted while plugins load, so nothing can match yet. The game's C
        // runtime startup calls GetStartupInfoW once the real code runs;
        // resolve and install from there (the way gigaHours' mm_sdk does).
        LPVOID gsi = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetStartupInfoW");
        MH_STATUS c = gsi ? MH_CreateHook(gsi, (LPVOID)GetStartupInfoW_hook, (LPVOID*)&GetStartupInfoW_orig) : MH_ERROR_FUNCTION_NOT_FOUND;
        MH_STATUS e = (c == MH_OK) ? MH_EnableHook(gsi) : c;
        LogLine("game code not readable yet (Steam DRM?), installing at game startup: %s", MH_StatusToString(e));
        return TRUE;
    }
    InstallMod(ok);
    return TRUE;
}

static void InstallMod(bool ok) {
    LogLine("game check: %s", g_versionStatus);
    for (int i = 0; i < SIG_COUNT; i++)
        LogLine("  %-60s %s (matches %d)%s", g_sigs[i].name, g_sigs[i].matches == 1 ? "OK" : "--", g_sigs[i].matches,
            g_sigs[i].alreadyHooked ? " -- already hooked by another mod, chaining behind it" : "");
    if (!ok) {
        char msg[600];
        snprintf(msg, sizeof(msg),
            "Wasteland Storms is DISABLED: it could not find the game functions it needs in this executable.\n\n"
            "Reason: %s\n\n"
            "The mod stays loaded but does nothing, so the game is safe to play. "
            "Please report your game version (store + patch) to the mod author.", g_versionStatus);
        MessageBoxA(NULL, msg, "Mad Max - Wasteland Storms", MB_OK | MB_ICONWARNING);
        return;
    }

    LoadSettings();
    SendEventMsg = (SendMsgFn)g_sigs[SIG_SENDMSG].resolved;
    CGameObject_FindOptional = (FindOptionalFn)g_sigs[SIG_FINDOPTIONAL].resolved;

    LogLine("frame hook: %s", InstallHook(g_sigs[SIG_UPDATECONTROLLER].resolved, (LPVOID)UpdateController_hook, (LPVOID*)&UpdateController_orig));
    LogLine("weather manager hook: %s", g_sigs[SIG_TOD_RENDER].matches == 1
        ? InstallHook(g_sigs[SIG_TOD_RENDER].resolved, (LPVOID)TodInternalUpdateRender_hook, (LPVOID*)&TodInternalUpdateRender_orig)
        : "signature not found, using the object lookup");
    g_tagNoStorm = JenkinsHash("no_storm");
    if (g_sigs[SIG_TAGREGISTER].matches == 1) {
        uintptr_t insn = g_sigs[SIG_TAGREGISTER].resolved + 7;          // mov rcx,[rip+disp32]
        g_envManagerGlobal = (void**)(insn + 7 + *(int32_t*)(insn + 3));
        uintptr_t jmp = g_sigs[SIG_TAGREGISTER].resolved + 0x1C;          // E9 rel32 -> RegisterActivationTagsCallback
        if (*(uint8_t*)jmp == 0xE9) RegisterTagCallback = (RegisterTagCallbackFn)(jmp + 5 + *(int32_t*)(jmp + 1));
        LogLine("environment tags: register function %s", RegisterTagCallback ? "found" : "NOT found (no jmp at +0x1C)");
        LogLine("environment tags: ready (no_storm = 0x%08X)", g_tagNoStorm);
    } else {
        LogLine("environment tags: signatures not found, the startup notice only checks the weather");
    }
    LogLine("ready: press the hotkey in game to choose how often storms come");
}
