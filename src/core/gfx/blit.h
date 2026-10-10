// Minimal alpha-blend textured blit (session 19): draws `src` over `dst` as a
// fullscreen triangle with straight-alpha blending. Used to composite the
// captured HUD back onto the flat window AFTER the XR eye capture - the one
// consumer ImGui cannot serve (the overlay draws before the capture, so an
// ImGui composite would put the HUD back into both eyes).
//
// Pipeline objects are created lazily from the context's device on first use
// (D3DCompile, like imgui_impl_dx11); full state backup/restore around the
// draw. Render thread only.

#pragma once

#include <d3d11.h>

namespace bvr::blit {

// Draw src over the full dst viewport (dstW x dstH) with PREMULTIPLIED
// blending (ONE / INV_SRC_ALPHA). The source is expected to carry sane alpha
// (see process below). Returns false if pipeline creation failed (logged once).
bool alpha_premul(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH);

// Alpha-repair pass, blending OFF: writes src.rgb unchanged (gameswf output
// over a zero-cleared RT is premultiplied by construction) with alpha =
// max(stored, luminance-derived). gameswf's blend states leave garbage in the
// destination alpha channel, so coverage must be reconstructed before any
// alpha-blended consumer (the window composite, the XR quad) can show it.
bool process(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
             ID3D11ShaderResourceView* src, UINT dstW, UINT dstH);
// SVRGLOW: alpha = max(alpha, max(r, g, b)), blending off - additive (alpha 0) pieces stay visible on runtimes that
// drop premultiplied colour with zero alpha (SteamVR).
bool alpha_repair_full(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                       ID3D11ShaderResourceView* src, UINT dstW, UINT dstH);

// Stretch a horizontal BAND of src ([topFrac .. topFrac + heightFrac] in v)
// across the FULL destination, blending off.
//
// UNUSED since session 29, and do NOT reintroduce it as a letterbox fix. It
// was written as the cinematic "unsqueeze" on session 22's reading that the
// engine squeezed cutscene content into a middle band. That reading is
// disproven: the bars are a gameswf draw over a FULL-FRAME image (ENGINE_NOTES
// session 29), so stretching a band crops real picture. Kept only as a generic
// UV-remap blit for some future caller that genuinely wants one.
bool stretch_band(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH,
                  float topFrac, float heightFrac);

// Batman City REFLECTEYE: copy src into dst (full viewport) sampling at u + du (clamped), blending off.
bool shift_copy(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, float du);

// Batman City SCREENROOM: clear dst to black and draw src on a screen whose four corners are
// given in dst's clip space (x, y, z, w each; order top-left, top-right, bottom-left,
// bottom-right). Blending off.
bool screen_in_eye(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                   ID3D11ShaderResourceView* src, const float corners[4][4]);

// Batman City ROOMGRUNGE: lay a captured full-screen overlay onto a screen in the room, inside one
// eye's image. The game's overlay draw was replayed twice, onto black and onto white. Any blend
// that is linear in the destination (add, multiply, alpha over, screen) then reads
// dst' = black + (white - black) * dst, which this draws with dual-source blending (rgb only, the
// scene's alpha is kept). Corners as screen_in_eye; the viewport is (0, 0, dstW, dstH).
bool overlay_in_eye(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                    ID3D11ShaderResourceView* blackSrv, ID3D11ShaderResourceView* whiteSrv,
                    const float corners[4][4]);

// Batman City PAUSEDIRT: fill dst (full viewport) with src as seen on a sphere around the recentred head.
// For each pixel the view ray is rebuilt with invClip (this eye's clip space -> the room space, row
// vectors, x right y up z forward) and met with the sphere of radius screen[0] centred screen[3] above
// the origin; the picture spans screen[1] x screen[2] radians of yaw x pitch. Mirror addressing
// continues it past its borders. Blending off.
bool plane_warp(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, const float invClip[16], const float screen[4]);

// Batman City PAUSEHALO: (1) dst (small) = src alpha averaged over 8x8 texel blocks; (2) dst = src with a soft dark
// halo under its content, spread from the small alpha picture. params: radius in uv (x, y), gain, strength. The
// source is premultiplied (the HUD capture), so the halo only raises alpha. Blending off.
bool alpha_down(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, UINT srcW, UINT srcH);
// Batman City WIPEREPLAY: src (premultiplied) over the whole dst viewport, enlarged by `zoom` about the centre.
bool zoom_premul(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                 ID3D11ShaderResourceView* src, float zoom);
// ... src alpha into a one-channel (R8) picture, blending off; and an R8 coverage picture drawn as black over dst.
bool alpha_to_r8(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                 ID3D11ShaderResourceView* src);
bool mask_black(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* mask, float zoom, bool invert = false); // invert: black where the mask is clear
bool halo_under(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, ID3D11ShaderResourceView* smallAlpha, const float params[4]);

// Release device objects (device loss / shutdown; recreated lazily).
void release();

} // namespace bvr::blit
