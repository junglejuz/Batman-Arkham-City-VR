#include "game/arkhamcity/billboard.h"

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bvr::ac::billboard {
namespace {

std::atomic<bool> g_on{true};
std::atomic<uint32_t> g_patched{0};

constexpr uint32_t kSlots = 64;
struct Pass {
    std::atomic<uint32_t> seq{0};
    float eye[3][3];  // eye view rotation (world -> view), row vectors
    float map[3][3];  // eye frame -> game camera frame: v' = v * map
};
Pass g_pass[kSlots];
std::atomic<uint32_t> g_renderSlot{0};

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

void set_pass(uint32_t slot, const float eyeView[16], const float baseView[16]) {
    Pass& p = g_pass[slot % kSlots];
    p.seq.store(0, std::memory_order_relaxed);
    float E[3][3], B[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            E[i][j] = eyeView[i * 4 + j];
            B[i][j] = baseView[i * 4 + j];
        }
    memcpy(p.eye, E, sizeof(E));
    // map = E * B^T (both orthonormal rotations)
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) p.map[i][j] = E[i][0] * B[j][0] + E[i][1] * B[j][1] + E[i][2] * B[j][2];
    p.seq.store(slot + 1, std::memory_order_release);
}

void set_render_slot(uint32_t slot) { g_renderSlot.store(slot, std::memory_order_relaxed); }

void on_upload(ID3D11Resource* dst, const void* data) {
    if (!g_on.load(std::memory_order_relaxed) || !dst || !data) return;
    // Size first (the tap does not pass it): only whole 320- or 640-byte constant buffers are read.
    ID3D11Buffer* b = nullptr;
    if (FAILED(dst->QueryInterface(IID_PPV_ARGS(&b))) || !b) return;
    D3D11_BUFFER_DESC bd{};
    b->GetDesc(&bd);
    b->Release();
    if ((bd.ByteWidth != 320 && bd.ByteWidth != 640) || !(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    // Floats 12-14 and 16-18 unit length, w = 0, perpendicular.
    float* f = const_cast<float*>(static_cast<const float*>(data));
    const float l1 = dot3(f + 12, f + 12), l2 = dot3(f + 16, f + 16);
    if (fabsf(l1 - 1.0f) > 0.01f || fabsf(l2 - 1.0f) > 0.01f || f[15] != 0.0f || f[19] != 0.0f ||
        fabsf(dot3(f + 12, f + 16)) > 0.01f)
        return;
    const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
    const Pass& p = g_pass[slot % kSlots];
    if (p.seq.load(std::memory_order_acquire) != slot + 1) return;
    // They must be this eye's up (column 1) and right (column 0) axes, either sign.
    const float right[3] = {p.eye[0][0], p.eye[1][0], p.eye[2][0]};
    const float up[3] = {p.eye[0][1], p.eye[1][1], p.eye[2][1]};
    if (fabsf(fabsf(dot3(f + 16, right)) - 1.0f) > 0.002f || fabsf(fabsf(dot3(f + 12, up)) - 1.0f) > 0.002f) return;
    for (int k : {12, 16}) {
        const float v[3] = {f[k], f[k + 1], f[k + 2]};
        for (int j = 0; j < 3; ++j) f[k + j] = v[0] * p.map[0][j] + v[1] * p.map[1][j] + v[2] * p.map[2][j];
    }
    g_patched.fetch_add(1, std::memory_order_relaxed);
}

bool enabled() { return g_on.load(); }
void set_enabled(bool on) { g_on.store(on); }
uint32_t patched() { return g_patched.load(); }

} // namespace bvr::ac::billboard
