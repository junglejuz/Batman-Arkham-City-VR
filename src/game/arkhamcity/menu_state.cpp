#include "game/arkhamcity/menu_state.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bvr::ac::menu_state {
namespace {
namespace ps = bvr::pattern_scan;
#define PFX "[bc]"

// --- the game's name table (GNames), found live ------------------------------------------------------------
// UE3 keeps every FName's text in a TArray<FNameEntry*> { data, num, max } in a writable section. It is recognised by
// entry 0 = "None" and entry 1 = "ByteProperty" at one text offset inside the entry. Read fresh on every lookup (the
// array grows and may move).
const uint8_t* g_namesArr = nullptr;
int g_textOff = -1;
int g_objNameOff = -1; // UObject: FName index offset; the class pointer is 8 bytes after it (UE3: Name, then Class)

bool text_is(const uint8_t* p, const char* s, bool wide) {
    const size_t n = strlen(s) + 1;
    if (!ps::is_memory_valid(p, wide ? 2 * n : n)) return false;
    for (size_t i = 0; i < n; ++i)
        if ((wide ? reinterpret_cast<const uint16_t*>(p)[i] : p[i]) != uint8_t(s[i])) return false;
    return true;
}

bool g_wideNames = false; // FNameEntry text as UTF-16 (BioShock Infinite: +0x10 wide)
bool names_ok() { return g_namesArr && g_textOff >= 0; }

// Copies the text of name idx into out (always terminated); false when it is not a valid name.
bool name_text(int idx, char* out, size_t n) {
    out[0] = 0;
    if (!names_ok() || idx < 0) return false;
    uint8_t** data = *reinterpret_cast<uint8_t** const*>(g_namesArr);
    const int num = *reinterpret_cast<const int*>(g_namesArr + 4);
    if (idx >= num || !ps::is_memory_valid(data + idx, 4)) return false;
    const uint8_t* e = data[idx];
    if (!e) return false;
    const uint8_t* t = e + g_textOff;
    const size_t cw = g_wideNames ? 2 : 1;
    size_t i = 0;
    for (; i + 1 < n; ++i) {
        if (!ps::is_memory_valid(t + i * cw, cw)) return false;
        const unsigned c = g_wideNames ? reinterpret_cast<const uint16_t*>(t)[i] : t[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
        out[i] = static_cast<char>(c);
    }
    out[i] = 0;
    return i > 0 && i + 1 < n;
}

// MS2: page by page (a section can span several memory regions - the first version skipped whole 64 KB blocks that
// crossed one, and found nothing in City), ANSI or UTF-16 text, retried by the worker until found.
bool find_names(const ps::ProcessImage& img, bool logMiss) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* lo = img.base + sec->VirtualAddress;
        const size_t len = sec->Misc.VirtualSize & ~size_t(3);
        for (size_t page = 0; page < len; page += 0x1000) {
            if (!ps::is_memory_valid(lo + page, 0x1000 < len - page ? 0x1000 : len - page)) continue;
            const size_t end = page + 0x1000 < len ? page + 0x1000 : len;
            for (size_t o = page; o < end && o + 12 <= len; o += 4) {
                const uint8_t* p = lo + o;
                if (o + 12 > end && !ps::is_memory_valid(p, 12)) break; // the last dwords reach into the next page
                const int num = *reinterpret_cast<const int*>(p + 4), max = *reinterpret_cast<const int*>(p + 8);
                if (num < 2000 || num > 3000000 || max < num || max > 4000000) continue;
                uint8_t** data = *reinterpret_cast<uint8_t** const*>(p);
                if (!ps::is_memory_valid(data, 8)) continue;
                const uint8_t* e0 = data[0];
                const uint8_t* e1 = data[1];
                if (!ps::is_memory_valid(e0, 8) || !ps::is_memory_valid(e1, 8)) continue;
                for (int wide = 0; wide < 2; ++wide)
                    for (int off = 0x4; off <= 0x20; off += 4) {
                        if (text_is(e0 + off, "None", wide) && text_is(e1 + off, "ByteProperty", wide)) {
                            g_wideNames = wide != 0;
                            g_textOff = off;
                            g_namesArr = p;
                            BVR_LOG(PFX " MENUSTATE: name table at %08X (%d names, %s text at entry +0x%X)",
                                    unsigned(uintptr_t(p)), num, wide ? "UTF-16" : "ANSI", off);
                            return true;
                        }
                    }
            }
        }
    }
    if (logMiss) BVR_LOG(PFX " MENUSTATE: name table not found yet - calls are logged with addresses only");
    return false;
}

// The object's name and class name, from the first live object: the offset where Name, Class -> Name and
// Class -> Class -> Name ("Class") all resolve.
void find_object_layout(const uint8_t* obj) {
    if (g_objNameOff >= 0 || !names_ok() || !ps::is_memory_valid(obj, 0x80)) return;
    char t[96];
    for (int off = 0x10; off <= 0x60; off += 4) {
        const uint8_t* cls = *reinterpret_cast<const uint8_t* const*>(obj + off + 8);
        if (!name_text(*reinterpret_cast<const int*>(obj + off), t, sizeof t) || !ps::is_memory_valid(cls, 0x80)) continue;
        const uint8_t* meta = *reinterpret_cast<const uint8_t* const*>(cls + off + 8);
        if (!name_text(*reinterpret_cast<const int*>(cls + off), t, sizeof t) || !ps::is_memory_valid(meta, 0x80)) continue;
        if (name_text(*reinterpret_cast<const int*>(meta + off), t, sizeof t) && strcmp(t, "Class") == 0) {
            g_objNameOff = off;
            BVR_LOG(PFX " MENUSTATE: object name at +0x%X, class at +0x%X", off, off + 8);
            return;
        }
    }
}

void describe(const void* p, char* out, size_t n) {
    const uint8_t* obj = static_cast<const uint8_t*>(p);
    if (!obj) { snprintf(out, n, "none"); return; }
    find_object_layout(obj);
    char name[96] = "?", cls[96] = "?";
    if (g_objNameOff >= 0 && ps::is_memory_valid(obj, g_objNameOff + 12)) {
        name_text(*reinterpret_cast<const int*>(obj + g_objNameOff), name, sizeof name);
        const uint8_t* c = *reinterpret_cast<const uint8_t* const*>(obj + g_objNameOff + 8);
        if (ps::is_memory_valid(c, g_objNameOff + 4)) name_text(*reinterpret_cast<const int*>(c + g_objNameOff), cls, sizeof cls);
        const int number = *reinterpret_cast<const int*>(obj + g_objNameOff + 4);
        if (number > 0) {
            const size_t k = strlen(name);
            snprintf(name + k, sizeof name - k, "_%d", number - 1);
        }
    }
    snprintf(out, n, "%s %s (%08X)", cls, name, unsigned(uintptr_t(obj)));
}

// --- the menu natives ----------------------------------------------------------------------------------------
// Found by name in the live UE3 native table { "<Class>exec<Func>", impl } (City: 2630 entries) and hooked as one batch.
// LIVE 2026-10-11/12: pause = RGFxMovieUI_Pause (Start, SetFocus, RestoreMenuState, ... Close); the map and the
// upgrades = RGFxMovieBackScreen (Start + SetFocus ... Close); the loading screen Starts with focus while the full-screen
// movie flag is TRUE; the HUD (RGFxMovieHudExtendable / FrontMost) never counts.
enum Kind { kCall, kGetBool };
struct Target {
    const char* cls;
    const char* fn;
    Kind kind;
    void* orig = nullptr;
    void* impl = nullptr;
    uint32_t last = 0xFFFFFFFF; // getter: last answer
};
Target g_t[] = {
    {"UGFxMoviePlayer", "Start", kCall},
    {"UGFxMoviePlayer", "Close", kCall},
    {"UGFxMoviePlayer", "SetFocus", kCall},
    {"ARPlayerController", "IsPlayingFullScreenMovie", kGetBool},
    {"URGFxMovieUI", "RestoreMenuState", kCall},
};
enum { kStart = 0, kClose = 1, kSetFocus = 2, kFullScreen = 3, kRestoreMenu = 4 }; // indices into g_t
constexpr int kTargets = int(sizeof(g_t) / sizeof(g_t[0]));
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<int> g_hooked{0};

// A menu = a movie that Starts and takes focus in the same moment, until it is Closed. Menus opened together close
// together (Origins' map opens three movies and closes two).
struct OpenMenu { const void* obj; ULONGLONG at; bool pause; char cls[48]; };
OpenMenu g_open[8] = {};
int g_openN = 0;
const void* g_started = nullptr;
ULONGLONG g_startedMs = 0;
std::atomic<bool> g_menuOpen{false};
std::atomic<ULONGLONG> g_menuOpenAt{0}, g_menuClosedAt{0};
std::atomic<bool> g_lastOpenBack{false}; // TABRACE: the last menu opened is the Tab screen (map / upgrades)
void menu_track(int i, const void* self) { // under g_lock
    const ULONGLONG now = GetTickCount64();
    if (i == kStart) { g_started = self; g_startedMs = now; return; }
    if (i == kSetFocus && self == g_started && now - g_startedMs < 100 && g_t[kFullScreen].last != 1) {
        g_started = nullptr;
        for (int k = 0; k < g_openN; ++k) if (g_open[k].obj == self) return;
        char who[160];
        describe(self, who, sizeof who);
        char cls[48] = {};
        for (int k = 0; k < 47 && who[k] && who[k] != ' '; ++k) cls[k] = who[k];
        if (strstr(cls, "Hud") || strstr(cls, "FrontMost")) return; // the HUD, not a menu
        if (g_openN < 8) {
            OpenMenu& m = g_open[g_openN++];
            m = OpenMenu{self, now, strstr(cls, "Pause") != nullptr, {}};
            memcpy(m.cls, cls, sizeof cls);
        }
        g_lastOpenBack.store(strstr(cls, "BackScreen") != nullptr);
        g_menuOpen.store(true);
        g_menuOpenAt.store(now);
        BVR_LOG(PFX " menu open: %s", who);
        return;
    }
    if (i == kRestoreMenu) {
        for (int k = 0; k < g_openN; ++k) if (g_open[k].obj == self) g_open[k].pause = true;
        return;
    }
    if (i == kClose) {
        for (int k = 0; k < g_openN; ++k) {
            if (g_open[k].obj != self) continue;
            BVR_LOG(PFX " menu closed: %s after %llu ms", g_open[k].cls, now - g_open[k].at);
            const ULONGLONG openedAt = g_open[k].at;
            g_open[k] = g_open[--g_openN];
            for (int j = 0; j < g_openN;) {
                const ULONGLONG d = g_open[j].at > openedAt ? g_open[j].at - openedAt : openedAt - g_open[j].at;
                if (d <= 300) g_open[j] = g_open[--g_openN]; else ++j;
            }
            g_menuOpen.store(g_openN > 0);
            g_menuClosedAt.store(now);
            return;
        }
    }
}

void note(int i, void* self, void* result) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_t[i].kind == kGetBool) {
        if (result && ps::is_memory_valid(result, 4)) g_t[i].last = *static_cast<const uint32_t*>(result);
    } else {
        menu_track(i, self);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

using ExecFn = void(__fastcall*)(void* self, void* edx, void* stack, void* result);
template <int I>
void __fastcall exec_detour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_t[I].orig)(self, edx, stack, result);
    note(I, self, result);
}
template <int... I>
struct DetourTable { static inline void* const fns[] = {reinterpret_cast<void*>(&exec_detour<I>)...}; };
using Detours = DetourTable<0, 1, 2, 3, 4>;
static_assert(sizeof(Detours::fns) / sizeof(void*) == kTargets, "one detour per target");

// How many native-table entries use this impl (a shared stub would see other natives' calls too).
int table_users(const ps::ProcessImage& img, const void* impl) {
    int users = 0;
    for (const uint8_t* ref : ps::find_references(img, impl)) {
        if (!ps::is_memory_valid(ref - 4, 4)) continue;
        const uint8_t* s = *reinterpret_cast<const uint8_t* const*>(ref - 4);
        char buf[96] = {};
        if (!ps::is_memory_valid(s, 8)) continue;
        for (int k = 0; k < 95 && ps::is_memory_valid(s + k, 1) && s[k]; ++k) buf[k] = static_cast<char>(s[k]);
        if (strstr(buf, "exec")) ++users;
    }
    return users;
}

DWORD WINAPI install_worker(void*) {
    ps::ProcessImage img;
    if (!ps::capture_main_module(img)) return 0;
    int queued = 0;
    for (int i = 0; i < kTargets; ++i) {
        Target& t = g_t[i];
        ps::NativeScanResult r;
        if (!ps::find_native_function_ex(img, ps::kNativeTableUE3, t.cls, t.fn, r, true) || !r.function ||
            table_users(img, r.function) > 1) {
            BVR_LOG(PFX " menu signal: %sexec%s not usable", t.cls, t.fn);
            continue;
        }
        if (MH_CreateHook(r.function, Detours::fns[i], &t.orig) != MH_OK || MH_QueueEnableHook(r.function) != MH_OK) continue;
        t.impl = r.function;
        ++queued;
    }
    if (queued && MH_ApplyQueued() == MH_OK) g_hooked.store(queued);
    BVR_LOG(PFX " menu signal: %d of %d game menu functions hooked", g_hooked.load(), kTargets);
    // The name table may not exist yet this early: retried (names are only for the menu types and the log).
    for (int k = 0; k < 40 && !names_ok(); ++k) {
        Sleep(3000);
        find_names(img, k == 39);
    }
    return 0;
}

} // namespace

// The scans read the whole game image: own thread, never the game's.
void install() {
    if (HANDLE t = CreateThread(nullptr, 0, install_worker, nullptr, 0, nullptr)) CloseHandle(t);
}

void status_line(char* out, unsigned n) {
    snprintf(out, n, "menu signal: %d game menu functions hooked, name table %s, menu %s", g_hooked.load(),
             names_ok() ? "found" : "missing", g_menuOpen.load() ? "open" : "closed");
}

bool live() {
    return g_hooked.load(std::memory_order_relaxed) > 0 && g_t[kStart].impl && g_t[kClose].impl && g_t[kSetFocus].impl;
}
bool menu_open() { return g_menuOpen.load(std::memory_order_relaxed); }
unsigned long long menu_open_at() { return g_menuOpenAt.load(std::memory_order_relaxed); }
unsigned long long menu_closed_at() { return g_menuClosedAt.load(std::memory_order_relaxed); }
bool last_open_is_back_screen() { return g_lastOpenBack.load(std::memory_order_relaxed); }
bool name_of(int idx, char* out, unsigned n) { return n && name_text(idx, out, n); }

void clear_stale(const char* why) {
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_openN;
    g_openN = 0;
    g_menuOpen.store(false);
    ReleaseSRWLockExclusive(&g_lock);
    if (n) BVR_LOG(PFX " menu state cleared (%d open) - %s", n, why);
}

} // namespace bvr::ac::menu_state
