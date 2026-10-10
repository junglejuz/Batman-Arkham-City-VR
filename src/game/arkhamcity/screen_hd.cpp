// Batman City HD SCREENS - see screen_hd.h and BATMAN_CITY_PLAN.md "HD SCREENS".
#include "game/arkhamcity/screen_hd.h"
#include "core/gfx/blit.h"
#include "core/gfx/frame_inspector.h"
#include "game/arkhamcity/vision.h"
#include "game/arkhamcity/window.h"
#include "core/util/log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bvr::ac::screen_hd {
namespace {

std::atomic<bool> g_on{true};    // swap in the upscaled pictures that exist
std::atomic<bool> g_dump{true};  // save every new loading-screen picture once
std::atomic<uint32_t> g_seen{0}, g_dumped{0}, g_loaded{0}, g_swaps{0}, g_skipped{0};

struct Entry {
    ID3D11Resource* res;  // identity only (not referenced): checked together with the size and format
    UINT w, h;
    DXGI_FORMAT fmt;
    uint32_t key;
    ID3D11ShaderResourceView* hd;
};
constexpr int kMax = 1024;
Entry g_e[kMax] = {};
int g_n = 0;
ID3D11ShaderResourceView* g_restore = nullptr; // the game's picture while ours is bound
bool g_bound = false;

IWICImagingFactory* wic() {
    static IWICImagingFactory* f = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // S_FALSE / RPC_E_CHANGED_MODE: already set up
        (void)ci;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) f = nullptr;
        if (!f) BVR_LOG("[hd] Windows imaging component unavailable - no PNG load / save");
    }
    return f;
}

bool save_png(const wchar_t* path, const uint8_t* rgba, UINT w, UINT h, UINT pitch) {
    IWICImagingFactory* f = wic();
    if (!f) return false;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path, GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h));
    if (ok) {
        // The PNG encoder takes BGRA (it answered 32bppRGBA with another format: the first dump wrote empty files)
        std::vector<uint8_t> bgra(size_t(pitch) * h);
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
            bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = rgba[i + 3];
        }
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(fr->WritePixels(h, pitch, pitch * h, bgra.data())) && SUCCEEDED(fr->Commit()) &&
             SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (s) s->Release();
    if (!ok) DeleteFileW(path); // never leave an empty file (it would block the next dump)
    return ok;
}

bool load_png(const wchar_t* path, std::vector<uint8_t>& px, UINT& w, UINT& h) {
    IWICImagingFactory* f = wic();
    if (!f || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192;
    if (ok) {
        px.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return ok;
}

ID3D11ShaderResourceView* load_hd(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t key) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* sl = wcsrchr(path, L'\\')) *sl = 0;
    wchar_t file[MAX_PATH];
    swprintf_s(file, L"%s\\BatmanCityData\\hd\\%08x.png", path, key);
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    if (!load_png(file, px, w, h)) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 0; // full chain, generated below (the loading screen scales its pictures)
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* t = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(dev->CreateShaderResourceView(t, nullptr, &srv))) {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->UpdateSubresource(t, 0, nullptr, px.data(), w * 4, 0);
        ctx->GenerateMips(srv);
    }
    if (t) t->Release();
    if (srv) {
        g_loaded.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("[hd] %08x: upscaled picture %ux%u loaded", key, w, h);
    }
    return srv;
}

// A picture worth upscaling: an ordinary image (not a render target, not rewritten every frame like video planes or
// the font cache), colour (not a one-channel mask), at least 256 px on its long side.
bool candidate(const D3D11_TEXTURE2D_DESC& d) {
    if (d.Usage == D3D11_USAGE_DYNAMIC || (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL |
                                                          D3D11_BIND_UNORDERED_ACCESS)))
        return false;
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1) return false;
    switch (d.Format) {
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        return false;
    default: break;
    }
    const UINT lo = d.Width < d.Height ? d.Width : d.Height, hi = d.Width > d.Height ? d.Width : d.Height;
    return hi >= 256 && lo >= 32 && hi <= 4096;
}

// SYNOPSIS (JJ 2026-10-10, the loading-screen slideshow): the 87 Synopsis*_SF pictures (1024x512) were exported with
// UModel and upscaled by Magnific (4 per request, precision, sharpness 0) into
// <game>\BatmanCityData\hd\synopsis\<name>.png. The game's own block decode differs slightly from UModel's, so pictures are recognised by a 32x16 grid
// of mean brightness (synopsis_index.txt, same formula) - nearest match, mean difference < 5 (closest two pictures
// differ by 14.6).
ID3D11ShaderResourceView* load_named(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* name); // below
struct SynSig { char name[32]; uint8_t g[512]; };
std::vector<SynSig> g_syn;
bool g_synTried = false;
std::atomic<uint32_t> g_synMatched{0};
void syn_load() {
    if (g_synTried) return;
    g_synTried = true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* sl = wcsrchr(path, L'\\')) *sl = 0;
    wchar_t file[MAX_PATH];
    swprintf_s(file, L"%s\\BatmanCityData\\hd\\synopsis\\synopsis_index.txt", path);
    FILE* f = _wfopen(file, L"r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        SynSig sg{};
        char hex[1100] = {};
        if (sscanf_s(line, "%31s %1099s", sg.name, unsigned(sizeof(sg.name)), hex, unsigned(sizeof(hex))) != 2 || strlen(hex) != 1024) continue;
        for (int i = 0; i < 512; ++i) { unsigned v = 0; sscanf_s(hex + i * 2, "%2x", &v); sg.g[i] = uint8_t(v); }
        g_syn.push_back(sg);
    }
    fclose(f);
    BVR_LOG("[hd] loading-screen slideshow index: %u pictures", unsigned(g_syn.size()));
}

Entry* entry_for(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv) {
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    if (!r) return nullptr;
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(IID_PPV_ARGS(&t))) || !t) { r->Release(); return nullptr; }
    t->GetDesc(&d);
    t->Release();
    r->Release(); // identity only
    for (int i = 0; i < g_n; ++i)
        if (g_e[i].res == r && g_e[i].w == d.Width && g_e[i].h == d.Height && g_e[i].fmt == d.Format) return &g_e[i];
    if (!candidate(d)) return nullptr;
    if (g_n >= kMax) { g_skipped.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    Entry& e = g_e[g_n++];
    e = Entry{r, d.Width, d.Height, d.Format, 0, nullptr};
    g_seen.fetch_add(1, std::memory_order_relaxed);
    // Fingerprint: the picture decoded into plain RGBA8 (the originals are block compressed), read back once.
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return &e;
    D3D11_TEXTURE2D_DESC od{};
    od.Width = d.Width;
    od.Height = d.Height;
    od.MipLevels = 1;
    od.ArraySize = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    od.SampleDesc.Count = 1;
    od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC sd = od;
    sd.BindFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* out = nullptr;
    ID3D11Texture2D* stage = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    const char* synName = nullptr; // SYNOPSIS: the matched slideshow picture
    if (SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &out)) && SUCCEEDED(dev->CreateRenderTargetView(out, nullptr, &rtv)) &&
        SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
        bvr::frame_inspector::ScopedSuppress quiet;
        if (bvr::blit::shift_copy(ctx, rtv, d.Width, d.Height, srv, 0.0f)) {
            ctx->CopyResource(stage, out);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                uint32_t h = 0x811C9DC5u;
                for (UINT y = 0; y < d.Height; ++y) {
                    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                    for (UINT x = 0; x < d.Width * 4; ++x) h = (h ^ row[x]) * 0x01000193u;
                }
                e.key = h;
                if (g_dump.load(std::memory_order_relaxed)) {
                    wchar_t dir[MAX_PATH], path[MAX_PATH];
                    swprintf_s(dir, L"%s\\hd_dump", bvr::log::data_dir());
                    CreateDirectoryW(dir, nullptr);
                    swprintf_s(path, L"%s\\%08x_%ux%u.png", dir, h, d.Width, d.Height);
                    WIN32_FILE_ATTRIBUTE_DATA fa{};
                    const bool have = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
                    if (!have) {
                        std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4);
                        for (UINT y = 0; y < d.Height; ++y)
                            memcpy(px.data() + size_t(y) * d.Width * 4,
                                   static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * 4);
                        if (save_png(path, px.data(), d.Width, d.Height, d.Width * 4)) g_dumped.fetch_add(1, std::memory_order_relaxed);
                        else BVR_LOG("[hd] %08x: saving the picture FAILED", h);
                    }
                }
                if (d.Width == 1024 && d.Height == 512) { // SYNOPSIS: the brightness grid of this picture
                    syn_load();
                    uint8_t g[512];
                    for (int by = 0; by < 16; ++by)
                        for (int bx = 0; bx < 32; ++bx) {
                            uint64_t sum = 0;
                            for (int y = by * 32; y < by * 32 + 32; ++y) {
                                const uint8_t* q = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(bx) * 32 * 4;
                                for (int x = 0; x < 32; ++x, q += 4) sum += q[0] * 299u + q[1] * 587u + q[2] * 114u;
                            }
                            const unsigned v = unsigned((sum + 512000) / 1024000);
                            g[by * 32 + bx] = uint8_t(v > 255 ? 255 : v);
                        }
                    int best = -1;
                    unsigned bestD = ~0u;
                    for (size_t i = 0; i < g_syn.size(); ++i) {
                        unsigned dsum = 0;
                        for (int k = 0; k < 512; ++k) dsum += unsigned(abs(int(g[k]) - int(g_syn[i].g[k])));
                        if (dsum < bestD) { bestD = dsum; best = int(i); }
                    }
                    if (best >= 0 && bestD < 5u * 512u) synName = g_syn[size_t(best)].name;
                    if (best >= 0) BVR_LOG("[hd] slideshow picture: nearest %s, mean difference %.2f -> %s", g_syn[size_t(best)].name,
                                           bestD / 512.0, synName ? "HD version" : "no match");
                }
                ctx->Unmap(stage, 0);
            }
        }
    }
    if (synName) { // SYNOPSIS: the upscaled slideshow picture
        wchar_t wname[40] = {}, rel[80] = {};
        MultiByteToWideChar(CP_ACP, 0, synName, -1, wname, 40);
        swprintf_s(rel, L"synopsis\\%s.png", wname);
        e.hd = load_named(dev, ctx, rel);
        if (e.hd) g_synMatched.fetch_add(1, std::memory_order_relaxed);
    }
    if (!e.hd && e.key) e.hd = load_hd(dev, ctx, e.key);
    BVR_LOG("[hd] loading-screen picture %ux%u format %d: key %08x, upscaled version %s", d.Width, d.Height, int(d.Format),
            e.key, e.hd ? "LOADED" : "none");
    if (stage) stage->Release();
    if (rtv) rtv->Release();
    if (out) out->Release();
    dev->Release();
    return &e;
}

// HD LOGO state (present thread)
std::atomic<bool> g_logoOn{true};
std::atomic<int> g_logoOffsetMs{0};      // shift of the card times against the movie start (bc hd logooffset N)
ID3D11ShaderResourceView* g_logoSrv[2] = {}; // 0 title card, 1 tech-logos card
bool g_logoTried = false;
ULONGLONG g_logoStart = 0;
// LOGOFIT (JJ 2026-10-10: "you can see them rescaling when they appear"): the video's real place on screen, measured
// once per playback - the bright content of the title card on screen vs in the 1280x720 source (253,73)-(1044,678).
bool g_fitDone = false, g_fitOk = false;
float g_fit[4] = {}; // left, top, width, height of the 1280x720 video in backbuffer pixels
std::atomic<uint32_t> g_logoFrames{0};

ID3D11ShaderResourceView* load_named(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* name) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* sl = wcsrchr(path, L'\\')) *sl = 0;
    wchar_t file[MAX_PATH];
    swprintf_s(file, L"%s\\BatmanCityData\\hd\\%s", path, name);
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    if (!load_png(file, px, w, h)) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    ID3D11Texture2D* t = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, &init, &t))) dev->CreateShaderResourceView(t, nullptr, &srv);
    if (t) t->Release();
    BVR_LOG("[hd] logo card %ls: %s (%ux%u)", name, srv ? "loaded" : "FAILED", w, h);
    return srv;
}

} // namespace

void logo_frame(ID3D11DeviceContext* ctx, IDXGISwapChain* sc) {
    const bool playing = vision::movie_playing() && window::logo_video_opened();
    if (!playing) { g_logoStart = 0; g_fitDone = false; return; }
    const ULONGLONG now = GetTickCount64();
    if (!g_logoStart) g_logoStart = now; // the movie's first shown frame (poll_movie sees it start)
    if (!g_logoOn.load(std::memory_order_relaxed) || !ctx || !sc) return;
    // card times in video frames at 60 fps (bac_logo.usm, measured 2026-10-10): title fades in 0-10 (left to the video),
    // still 11-164; tech card 165-312; a few frames of margin at the cuts so a late / early start never shows a wrong card
    const long long f = (long long(now - g_logoStart) + g_logoOffsetMs.load(std::memory_order_relaxed)) * 60 / 1000;
    const int card = (f >= 11 && f <= 161) ? 0 : (f >= 168 && f <= 308) ? 1 : -1;
    if (card < 0) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    if (!g_logoTried) {
        g_logoTried = true;
        g_logoSrv[0] = load_named(dev, ctx, L"logo_title.png");
        g_logoSrv[1] = load_named(dev, ctx, L"logo_tech.png");
    }
    ID3D11Texture2D* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (g_logoSrv[card] && SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
        D3D11_TEXTURE2D_DESC bd{};
        bb->GetDesc(&bd);
        if (!g_fitDone && card == 0 && (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                        bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            g_fitDone = true; // the video's own title card is still in the frame: find its bright content once
            D3D11_TEXTURE2D_DESC sd = bd;
            sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1;
            ID3D11Texture2D* st = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
                bvr::frame_inspector::ScopedSuppress quiet;
                ctx->CopyResource(st, bb);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    const bool bgr = bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    int x0 = int(bd.Width), y0 = int(bd.Height), x1 = -1, y1 = -1;
                    for (UINT yy = 0; yy < bd.Height; ++yy) {
                        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(yy) * m.RowPitch;
                        for (UINT xx = 0; xx < bd.Width; ++xx) {
                            const uint8_t* q = row + xx * 4;
                            const int r = bgr ? q[2] : q[0], g = q[1], b = bgr ? q[0] : q[2];
                            if (r * 299 + g * 587 + b * 114 > 60000) {
                                if (int(xx) < x0) x0 = int(xx); if (int(xx) > x1) x1 = int(xx);
                                if (int(yy) < y0) y0 = int(yy); if (int(yy) > y1) y1 = int(yy);
                            }
                        }
                    }
                    ctx->Unmap(st, 0);
                    if (x1 > x0 && y1 > y0) {
                        const float sx = float(x1 - x0 + 1) / float(1044 - 253 + 1), sy = float(y1 - y0 + 1) / float(678 - 73 + 1);
                        g_fitOk = sx > 0.3f && sx < 5.0f && fabsf(sx / sy - 1.0f) < 0.06f;
                        const float sc = (sx + sy) * 0.5f;
                        g_fit[0] = float(x0) - 253.0f * sc; g_fit[1] = float(y0) - 73.0f * sc; g_fit[2] = 1280.0f * sc; g_fit[3] = 720.0f * sc;
                        BVR_LOG("[hd] logo video measured on screen: content (%d,%d)-(%d,%d), scale x %.3f y %.3f -> video %.0f,%.0f %.0fx%.0f %s",
                                x0, y0, x1, y1, sx, sy, g_fit[0], g_fit[1], g_fit[2], g_fit[3], g_fitOk ? "(used)" : "(NOT used: shapes disagree)");
                    } else {
                        BVR_LOG("[hd] logo video measure: no bright content in the frame yet - 16:9 band used");
                    }
                }
                st->Release();
            }
        }
        if (SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv) {
            float x = 1.0f, y = 1.0f, cx = 0.0f, cy = 0.0f;
            if (g_fitOk) { // the measured place, in clip space
                x = g_fit[2] / float(bd.Width); y = g_fit[3] / float(bd.Height);
                cx = (g_fit[0] + g_fit[2] * 0.5f) / float(bd.Width) * 2.0f - 1.0f;
                cy = 1.0f - (g_fit[1] + g_fit[3] * 0.5f) / float(bd.Height) * 2.0f;
            } else { // the movie stage is 16:9, centred in the window
                const float bandH = (float(bd.Width) * 9.0f / 16.0f) / float(bd.Height);
                y = bandH < 1.0f ? bandH : 1.0f; x = bandH < 1.0f ? 1.0f : 1.0f / bandH;
            }
            const float c[4][4] = {{cx - x, cy + y, 0.5f, 1.0f}, {cx + x, cy + y, 0.5f, 1.0f},
                                   {cx - x, cy - y, 0.5f, 1.0f}, {cx + x, cy - y, 0.5f, 1.0f}};
            bvr::frame_inspector::ScopedSuppress quiet;
            if (bvr::blit::screen_in_eye(ctx, rtv, bd.Width, bd.Height, g_logoSrv[card], c)) g_logoFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (rtv) rtv->Release();
    if (bb) bb->Release();
    dev->Release();
}

void on_draw(ID3D11DeviceContext* ctx, bool active) {
    if (!active || !ctx || (!g_on.load(std::memory_order_relaxed) && !g_dump.load(std::memory_order_relaxed))) return;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    const Entry* e = entry_for(ctx, srv);
    if (e && e->hd && g_on.load(std::memory_order_relaxed)) {
        ctx->PSSetShaderResources(0, 1, const_cast<ID3D11ShaderResourceView* const*>(&e->hd));
        g_restore = srv; // keeps the reference until after_draw
        g_bound = true;
        g_swaps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    srv->Release();
}

void after_draw(ID3D11DeviceContext* ctx) {
    if (!g_bound) return;
    g_bound = false;
    ctx->PSSetShaderResources(0, 1, &g_restore);
    if (g_restore) g_restore->Release();
    g_restore = nullptr;
}

bool command(const char* a) {
    auto starts = [&](const char* w) { return _strnicmp(a, w, strlen(w)) == 0; };
    if (starts("on")) g_on.store(true);
    else if (starts("off")) g_on.store(false);
    else if (starts("dump ")) g_dump.store(atoi(a + 5) != 0);
    else if (starts("logo ")) g_logoOn.store(atoi(a + 5) != 0);
    else if (starts("logooffset ")) g_logoOffsetMs.store(atoi(a + 11));
    else if (!starts("status")) return false;
    char s[300];
    status_line(s, sizeof(s));
    BVR_LOG("[hd] %s", s);
    return true;
}

void status_line(char* out, unsigned n) {
    snprintf(out, n, "HD screens %s, dump %s | pictures seen %u, saved %u, upscaled loaded %u, swaps %u, table full %u | logo cards %s, frames painted %u",
             g_on.load() ? "on" : "off", g_dump.load() ? "on" : "off", g_seen.load(), g_dumped.load(), g_loaded.load(),
             g_swaps.load(), g_skipped.load(), g_logoOn.load() ? "on" : "off", g_logoFrames.load());
}

} // namespace bvr::ac::screen_hd
