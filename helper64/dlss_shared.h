#pragma once
// Batman City DLSS hand-off (DLSS_PLAN.md): the 32-bit game and the 64-bit helper (bc_dlss64.exe) share this block
// (named file mapping kMapName) plus one texture set per eye (legacy shared handles + keyed mutexes: key 0 = the
// game holds it, key 1 = the helper holds it). Both sides compile this header; keep it plain and fixed-size.

#include <cstdint>

namespace bcdlss {

constexpr wchar_t kMapName[] = L"Local\\BatmanCityDLSS_v1";
constexpr wchar_t kReqName[] = L"Local\\BatmanCityDLSS_req";
constexpr wchar_t kDoneName[] = L"Local\\BatmanCityDLSS_done";
constexpr uint32_t kVersion = 4;

enum Tex : int { kColor = 0, kDepth = 1, kMotion = 2, kOut = 3, kTexCount = 4 };

#pragma pack(push, 8)
struct Shared {
    uint32_t version;
    uint32_t gamePid;
    uint32_t helperPid;
    volatile uint32_t helperReady;   // 1 once the helper's device is up
    uint32_t luidLow;                // the game's graphics adapter (helper makes its device there)
    int32_t luidHigh;
    // Texture set (written by the game; gen bumps whenever any handle changes).
    volatile uint32_t gen;
    uint64_t handle[2][kTexCount];   // legacy shared handles (valid in any process); 0 = not used
    uint32_t renderW, renderH, outW, outH;
    // One request (game -> helper), then its answer (helper -> game).
    int32_t eye;                     // 0 left, 1 right
    uint32_t frame;
    int32_t mode;                    // 0 copy (D0), 1 DLSS
    float jitterX, jitterY;          // render pixels
    float mvScaleX, mvScaleY;
    uint32_t reset;
    volatile uint32_t answered;      // request serial the helper finished
    uint32_t serial;                 // request serial
    int32_t status;                  // 0 ok, <0 failed
    float helperMs;                  // helper-side time for the request (CPU)
    char message[256];               // last helper message (for the game's log)
    // D1: DLSS settings (game -> helper; a change rebuilds the features)
    int32_t dlssMode;                // 0 copy, 1 DLAA, 2 ultra quality, 3 quality, 4 balanced, 5 performance
    int32_t depthInverted;           // NGX DepthInverted flag (1 = near is 1.0)
    int32_t preset;                  // 0 = DLSS default, else render preset hint (A=1 ...)
    float gpuMs;                     // helper: last DLSS evaluate on the graphics card (both eyes averaged)
    int32_t ngxState;                // helper: 0 not tried, 1 ready, -1 failed
    int32_t evalRepeat;              // test: extra DLSS evaluates per request (cost of DLSS itself vs the hand-off)
    // FINISHFIRST (2026-10-09, the Xid 13 freezes): how the hand-off waits. 0 = the graphics card waits (keyed
    // mutexes only: each side's context stalls mid-stream on the other's work); 1 = the helper's DLSS has run on the
    // card before it answers (the game's context never stalls); 2 = both sides' work has run before each hand-over
    // (no card-side waits at all, default).
    int32_t syncMode;
    float finishMs;                  // helper: CPU wait for its own work to finish on the card (last request)
    uint32_t finishTimeouts;         // helper: waits that hit their limit
    // OVERLAP (sync 3, 2026-10-09): both eyes in flight, one request slot per eye. The game copies an eye in,
    // signals fenceIn = the slot's serial and goes on drawing; the helper waits for fenceIn ON THE CPU (its context
    // never stalls on the card), runs DLSS, writes status + answered, then signals fenceOut = the same serial; the
    // game waits for fenceOut on the CPU once per frame, at the right eye, and copies both outputs out. Serials
    // grow across both slots (left before right), so fenceOut >= right's serial means both are done.
    uint64_t fenceInHandle;          // NT handles, already duplicated into the helper process (0 = none)
    uint64_t fenceOutHandle;
    struct EyeReq {
        volatile uint32_t serial;    // written last by the game
        float jitterX, jitterY;
        uint32_t reset;
        int32_t dlssMode;
        int32_t status;              // helper
        float helperMs;              // helper
        volatile uint32_t answered;  // helper: written before fenceOut is signalled
    } req[2];
    uint32_t fenceWaitTimeouts;      // helper: fenceIn waits that hit their limit (served anyway: keyed mutex order)
};
#pragma pack(pop)

} // namespace bcdlss
