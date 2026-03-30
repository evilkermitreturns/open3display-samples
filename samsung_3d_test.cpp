// SPDX-License-Identifier: MIT

// ============================================================================
// Samsung Odyssey 3D Test — LeiaSR lens activation + our lenticular interleaver
//
// Uses LeiaSR SDK for: lens activation, calibration data
// Uses Open3Display for: lenticular interleaving shader
// No eye tracking, no head tracking — just static 3D
//
// SPACE = toggle interleaving on/off
// ESC = exit (disables lens)
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cmath>
#include <new>
#include <spdlog/spdlog.h>

// LeiaSR SDK — lens activation + calibration
#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/world/display/display.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/weaver/Weaver.h"
#include "sr/weaver/WeaverTypes.h"

// Our lenticular compositor
#include "backends/weaving_compositor_d3d11.h"
#include "stereo_frame.h"
#include "display_types.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static bool s_running = true;
static bool s_lenticular = true;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
        s_running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_SPACE) {
        s_lenticular = !s_lenticular;
        spdlog::info("[TOGGLE] Lenticular: {}", s_lenticular ? "ON" : "OFF");
    }
    // UP/DOWN arrows adjust interval, LEFT/RIGHT adjust x0
    if (msg == WM_KEYDOWN && wp == VK_UP)    PostMessageW(hwnd, WM_USER + 1, 1, 0);
    if (msg == WM_KEYDOWN && wp == VK_DOWN)  PostMessageW(hwnd, WM_USER + 1, 2, 0);
    if (msg == WM_KEYDOWN && wp == VK_LEFT)  PostMessageW(hwnd, WM_USER + 1, 3, 0);
    if (msg == WM_KEYDOWN && wp == VK_RIGHT) PostMessageW(hwnd, WM_USER + 1, 4, 0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void fill_sbs(uint32_t* pixels, int w, int h, int pitch, int frame_num) {
    int half = w / 2;
    float t = frame_num * 0.02f;
    int box_cx = half / 2, box_cy = h / 2;
    int box_x = (int)(sinf(t) * 80.0f) + box_cx;
    int box_y = (int)(cosf(t * 0.7f) * 50.0f) + box_cy;
    int parallax = 12;

    for (int y = 0; y < h; y++) {
        uint32_t* row = pixels + y * pitch;
        for (int x = 0; x < w; x++) {
            bool left = (x < half);
            int lx = left ? x : (x - half);
            uint8_t g = (uint8_t)(lx * 60 / (half > 1 ? half : 1) + 20);
            uint32_t bg = left
                ? (0xFF000000 | (g << 16) | (g / 2 << 8) | 40)
                : (0xFF000000 | 40 | (g / 2 << 8) | (g << 16));
            int bx = box_x + (left ? -parallax : parallax);
            if (lx >= bx - 40 && lx < bx + 40 && y >= box_y - 40 && y < box_y + 40)
                bg = 0xFFFFFFFF;
            if ((lx == half / 2 && y > h / 2 - 20 && y < h / 2 + 20) ||
                (y == h / 2 && lx > half / 2 - 20 && lx < half / 2 + 20))
                bg = 0xFF00FF00;
            row[x] = bg;
        }
    }
}

int main() {
    printf("========================================\n");
    printf("  Samsung Odyssey 3D Test\n");
    printf("  LeiaSR lens + Open3Display interleaver\n");
    printf("  SPACE = toggle 3D    ESC = exit\n");
    printf("========================================\n\n");

    SetProcessDPIAware();
    int WIN_W = GetSystemMetrics(SM_CXSCREEN);
    int WIN_H = GetSystemMetrics(SM_CYSCREEN);
    printf("[DISPLAY] Primary: %dx%d\n", WIN_W, WIN_H);

    // ---- LeiaSR SDK: initialize context + activate lens ----
    SR::SRContext* sr_ctx = nullptr;
    SR::SwitchableLensHint* lens_hint = nullptr;

    try {
        sr_ctx = new(std::nothrow) SR::SRContext();
        if (!sr_ctx) {
            spdlog::warn("[LEIA] SR context allocation failed");
        } else {
            lens_hint = SR::SwitchableLensHint::create(*sr_ctx);
            // NOTE: do NOT call sr_ctx->initialize() here — must be called AFTER creating weaver
            // (per SDK example pattern). We'll initialize after extracting calibration.
            spdlog::info("[LEIA] SR context created, lens hint ready");
        }
    } catch (const std::exception& e) {
        spdlog::warn("[LEIA] SR init failed: {}", e.what());
        spdlog::info("[LEIA] Continuing without lens activation");
    } catch (...) {
        spdlog::warn("[LEIA] SR init failed (unknown error)");
        spdlog::info("[LEIA] Continuing without lens activation");
    }

    stereo::LenticularConfig lent{};
    lent.panel_width  = (float)WIN_W;
    lent.panel_height = (float)WIN_H;
    lent.interval = 19.6169f;  // Fallback defaults
    lent.slope    = 0.1021f;
    lent.x0       = 3.59f;

    // 2-view SBS quilt
    stereo::QuiltConfig quilt{};
    quilt.columns     = 2;
    quilt.rows        = 1;
    quilt.view_count  = 2;
    quilt.view_width  = WIN_W / 2;
    quilt.view_height = WIN_H;
    quilt.reverse_views = false;

    // ---- D3D11 setup ----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Samsung3DTest";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"Samsung3DTest",
        L"Samsung 3D Test (SPACE=toggle, ESC=exit)",
        WS_POPUP, 0, 0, WIN_W, WIN_H,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = WIN_W;
    scd.BufferDesc.Height = WIN_H;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate = {60, 1};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc = {1, 0};
    scd.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swap, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("FATAL: D3D11 creation failed: 0x%08X\n", (unsigned)hr);
        return 1;
    }

    // ---- Extract Samsung calibration via temp SR weaver ----
    // Creating a weaver triggers DimencoWeaving.dll to load EDID-matched calibration
    if (sr_ctx) {
        try {
            SR::IDX11Weaver1* temp_weaver = nullptr;
            WeaverErrorCode wrc = SR::CreateDX11Weaver(sr_ctx, ctx, hwnd, &temp_weaver);
            if (wrc == WeaverSuccess && temp_weaver) {
                // Now the calibration is loaded — read it
                float slant = Dimenco::Weaver::GetSlant();
                float pitch = Dimenco::Weaver::GetPx();
                float n     = Dimenco::Weaver::GetN();
                float d_n   = Dimenco::Weaver::GetDoN();

                spdlog::info("[LEIA] Calibration via temp weaver:");
                spdlog::info("       Slant = {:.6f}", slant);
                spdlog::info("       Pitch = {:.6f} px", pitch);
                spdlog::info("       N     = {:.6f}", n);
                spdlog::info("       D/N   = {:.6f} mm", d_n);

                if (pitch > 0.0f && std::isfinite(pitch) && std::isfinite(slant)) {
                    // Leia SDK pitch appears to already be in subpixels.
                    // Our shader multiplies pixel position by 3 internally for subpixel coords,
                    // so pass raw SDK pitch — don't multiply by 3 again.
                    lent.interval = pitch;
                    lent.slope    = slant;
                    lent.x0 = 0.0f;
                    spdlog::info("[LEIA] Samsung calibration: pitch={:.4f} (raw) -> interval={:.4f}, slope={:.6f}",
                        pitch, lent.interval, slant);
                } else {
                    spdlog::warn("[LEIA] Calibration still invalid — using fallback");
                }

                // Also get display info
                SR::Display* sr_display = SR::Display::create(*sr_ctx);
                if (sr_display) {
                    spdlog::info("[LEIA] Display: {}x{} physical, recommended view {}x{}",
                        sr_display->getResolutionWidth(), sr_display->getResolutionHeight(),
                        sr_display->getRecommendedViewsTextureWidth(),
                        sr_display->getRecommendedViewsTextureHeight());
                }

                temp_weaver->destroy();
            } else {
                spdlog::warn("[LEIA] Temp weaver creation failed (code {}), using fallback calibration", (int)wrc);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[LEIA] Calibration extraction failed: {}", e.what());
        } catch (...) {
            spdlog::warn("[LEIA] Calibration extraction failed (unknown)");
        }
    }
    // NOW initialize context and enable lens (after weaver created calibration data)
    if (sr_ctx) {
        try {
            sr_ctx->initialize();
            if (lens_hint) {
                lens_hint->enable();
                spdlog::info("[LEIA] Context initialized, lens ENABLED");
            }
        } catch (const std::exception& e) {
            spdlog::warn("[LEIA] Initialize/lens failed: {}", e.what());
        } catch (...) {
            spdlog::warn("[LEIA] Initialize/lens failed");
        }
    }

    spdlog::info("[CONFIG] Final lenticular: interval={:.4f} slope={:.6f} x0={:.2f} panel={}x{}",
        lent.interval, lent.slope, lent.x0, WIN_W, WIN_H);

    // Textures
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = WIN_W; td.Height = WIN_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* sbs_tex = nullptr;
    device->CreateTexture2D(&td, nullptr, &sbs_tex);

    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&td, nullptr, &staging);

    ID3D11ShaderResourceView* sbs_srv = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(sbs_tex, &srv_desc, &sbs_srv);

    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* bb_rtv = nullptr;
    device->CreateRenderTargetView(bb, nullptr, &bb_rtv);
    bb->Release();

    // Our lenticular compositor
    stereo::LenticularCompositorD3D11 lc;
    if (!lc.init(device)) {
        printf("FATAL: Lenticular compositor init failed\n");
        return 1;
    }
    printf("[OK] All initialized. Running.\n\n");

    // ---- Main loop ----
    int frame = 0;
    while (s_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!s_running) break;

        // Handle calibration adjustments
        MSG adj;
        while (PeekMessageW(&adj, hwnd, WM_USER + 1, WM_USER + 1, PM_REMOVE)) {
            if (adj.wParam == 1) { lent.interval += 0.5f; spdlog::info("[TUNE] interval={:.4f}", lent.interval); }
            if (adj.wParam == 2) { lent.interval -= 0.5f; if (lent.interval < 0.5f) lent.interval = 0.5f; spdlog::info("[TUNE] interval={:.4f}", lent.interval); }
            if (adj.wParam == 3) { lent.x0 -= 0.5f; spdlog::info("[TUNE] x0={:.2f}", lent.x0); }
            if (adj.wParam == 4) { lent.x0 += 0.5f; spdlog::info("[TUNE] x0={:.2f}", lent.x0); }
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped);
        if (SUCCEEDED(hr)) {
            fill_sbs((uint32_t*)mapped.pData, WIN_W, WIN_H, mapped.RowPitch / 4, frame);
            ctx->Unmap(staging, 0);
        }
        ctx->CopyResource(sbs_tex, staging);

        if (s_lenticular) {
            bool ok = lc.render(ctx, sbs_srv, lent, quilt, bb_rtv, WIN_W, WIN_H);
            if (frame < 3) spdlog::info("[RENDER] Lenticular frame {}: {}", frame, ok ? "OK" : "FAILED");
        } else {
            ID3D11Texture2D* bb_tex = nullptr;
            swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb_tex);
            if (bb_tex) { ctx->CopyResource(bb_tex, sbs_tex); bb_tex->Release(); }
        }

        swap->Present(1, 0);
        frame++;
    }

    // ---- Cleanup ----
    printf("\nShutting down...\n");
    if (lens_hint) {
        lens_hint->disable();
        printf("[LEIA] Lens disabled\n");
    }
    lc.shutdown();
    sbs_srv->Release();
    bb_rtv->Release();
    staging->Release();
    sbs_tex->Release();
    swap->Release();
    ctx->Release();
    device->Release();
    if (sr_ctx) {
        delete sr_ctx;  // Cleans up lens_hint too
        printf("[LEIA] SR context destroyed\n");
    }
    DestroyWindow(hwnd);
    printf("Done. %d frames.\n", frame);
    return 0;
}
