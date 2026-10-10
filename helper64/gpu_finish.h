#pragma once
// FINISHFIRST (DLSS_PLAN.md "Freeze analysis", 2026-10-09): wait on the CPU until everything a context has submitted
// so far has run on the graphics card - a fence when the device has one, else an event query. Used by both sides of
// the DLSS hand-off so that neither hands a picture over while its own work is still queued: the other side's
// keyed-mutex wait is then already satisfied and its context never stalls mid-stream on the graphics card.

#include <windows.h>
#include <d3d11_4.h>

namespace bcdlss {

struct GpuFinish {
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* c4 = nullptr;
    HANDLE ev = nullptr;
    UINT64 value = 0;
    ID3D11Query* query = nullptr;
    bool tried = false;

    const char* kind() const { return ev ? "fence" : query ? "event query" : "none"; }

    // true = finished within ms; false = timed out (or nothing to wait with).
    bool wait(ID3D11Device* dev, ID3D11DeviceContext* ctx, DWORD ms) {
        if (!tried) {
            tried = true;
            ID3D11Device5* d5 = nullptr;
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d5))) && d5) {
                if (SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence &&
                    SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c4))) && c4)
                    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                d5->Release();
            }
            if (!ev) {
                if (c4) { c4->Release(); c4 = nullptr; }
                if (fence) { fence->Release(); fence = nullptr; }
                D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
                if (FAILED(dev->CreateQuery(&qd, &query))) query = nullptr;
            }
        }
        const ULONGLONG deadline = GetTickCount64() + ms;
        if (ev) {
            c4->Signal(fence, ++value);
            ctx->Flush();
            // A wait that timed out earlier can still set the event late: re-check the value, not just the event.
            while (fence->GetCompletedValue() < value) {
                if (FAILED(fence->SetEventOnCompletion(value, ev))) return false;
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) return false;
                WaitForSingleObject(ev, DWORD(deadline - now));
            }
            return true;
        }
        if (!query) return false;
        ctx->End(query);
        BOOL done = FALSE;
        while (ctx->GetData(query, &done, sizeof(done), 0) != S_OK) {
            if (GetTickCount64() >= deadline) return false;
            SwitchToThread();
        }
        return true;
    }
};

} // namespace bcdlss
