// SPDX-License-Identifier: MIT

// ============================================================================
// Minimal 2-View Lenticular Test
//
// SBS stereo pattern → LenticularCompositorD3D11 → backbuffer
// Nothing else. No mode switching, no runtime, no complexity.
// Proves the interleaver works (or doesn't) on real hardware.
//
// ESC = exit
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "backends/weaving_compositor_d3d11.h"
#include "stereo_frame.h"
#include "display_types.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static bool s_running = true;

static bool s_lenticular = true;  // Start with interleaving ON

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
        s_running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_SPACE) {
        s_lenticular = !s_lenticular;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Fill a simple SBS stereo pattern: left eye blue-tinted, right eye red-tinted,
// bouncing white box with parallax, green crosshair at screen plane.
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

            // Tinted gradient background — blue for left, red for right
            uint8_t g = (uint8_t)(lx * 60 / (half > 1 ? half : 1) + 20);
            uint32_t bg = left
                ? (0xFF000000 | (g << 16) | (g / 2 << 8) | 40)    // Blue tint
                : (0xFF000000 | 40 | (g / 2 << 8) | (g << 16));   // Red tint

            // Bouncing box with parallax
            int bx = box_x + (left ? -parallax : parallax);
            if (lx >= bx - 40 && lx < bx + 40 && y >= box_y - 40 && y < box_y + 40)
                bg = 0xFFFFFFFF;

            // Green crosshair at zero parallax (screen plane)
            if ((lx == half / 2 && y > h / 2 - 20 && y < h / 2 + 20) ||
                (y == h / 2 && lx > half / 2 - 20 && lx < half / 2 + 20))
                bg = 0xFF00FF00;

            row[x] = bg;
        }
    }
}

int main() {
    printf("========================================\n");
    printf("  Lenticular 2-View Test\n");
    printf("  Samsung Odyssey 3D (primary)\n");
    printf("  SPACE = toggle interleaving on/off\n");
    printf("  ESC = exit\n");
    printf("========================================\n\n");

    SetProcessDPIAware();

    // Samsung Odyssey 3D: 3840x2160 landscape (primary display)
    int WIN_W = GetSystemMetrics(SM_CXSCREEN);
    int WIN_H = GetSystemMetrics(SM_CYSCREEN);

    // Samsung calibration — approximate values, panel dims MUST match output
    // Interval/slope/x0 won't be exact without SR SDK but the interleaving
    // pattern should still be uniform and fullscreen (not SBS split)
    stereo::LenticularConfig lent{};
    lent.interval     = 19.6169f;   // Approximate — real value from SR SDK later
    lent.slope        = 0.1021f;    // Approximate
    lent.x0           = 3.59f;      // Approximate
    lent.panel_width  = (float)WIN_W;   // MUST match actual output
    lent.panel_height = (float)WIN_H;   // MUST match actual output

    // 2-view SBS (left half = left eye, right half = right eye)
    stereo::QuiltConfig quilt{};
    quilt.columns     = 2;
    quilt.rows        = 1;
    quilt.view_count  = 2;
    quilt.view_width  = WIN_W / 2;
    quilt.view_height = WIN_H;
    quilt.reverse_views = false;

    printf("[CONFIG] Samsung Odyssey 3D: %dx%d\n", WIN_W, WIN_H);
    printf("[CONFIG] Lenticular: interval=%.4f slope=%.4f x0=%.2f panel=%dx%d\n",
           lent.interval, lent.slope, lent.x0, WIN_W, WIN_H);
    printf("[CONFIG] 2-view SBS: %dx%d per eye\n", quilt.view_width, quilt.view_height);

    // Create window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LenticularTest";
    RegisterClassExW(&wc);

    // Fullscreen popup on primary (Samsung)
    HWND hwnd = CreateWindowExW(0, L"LenticularTest",
        L"Lenticular 2-View Test (ESC to exit)",
        WS_POPUP, 0, 0, WIN_W, WIN_H,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    // D3D11 device + swap chain
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
    printf("[OK] D3D11 device created\n");

    // Create SBS texture (GPU default) + staging (CPU write)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = WIN_W;
    td.Height = WIN_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* sbs_tex = nullptr;
    device->CreateTexture2D(&td, nullptr, &sbs_tex);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&td, nullptr, &staging);

    if (!sbs_tex || !staging) {
        printf("FATAL: Texture creation failed\n");
        return 1;
    }
    printf("[OK] Textures created (%dx%d)\n", WIN_W, WIN_H);

    // Create SRV for the SBS texture
    ID3D11ShaderResourceView* sbs_srv = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(sbs_tex, &srv_desc, &sbs_srv);
    if (!sbs_srv) {
        printf("FATAL: SRV creation failed\n");
        return 1;
    }

    // Create backbuffer RTV
    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* bb_rtv = nullptr;
    device->CreateRenderTargetView(bb, nullptr, &bb_rtv);
    bb->Release();
    if (!bb_rtv) {
        printf("FATAL: RTV creation failed\n");
        return 1;
    }

    // Initialize OUR lenticular compositor directly — no runtime wrapper
    stereo::LenticularCompositorD3D11 lc;
    if (!lc.init(device)) {
        printf("FATAL: Lenticular compositor init failed\n");
        return 1;
    }
    printf("[OK] Lenticular compositor initialized\n\n");

    printf("\n[RUNNING] SBS -> Lenticular interleave on Samsung primary\n");
    printf("[RUNNING] Should see: ONE fullscreen image with fine stripes\n");
    printf("[RUNNING] Should NOT see: SBS split (two halves side by side)\n\n");

    // Main loop
    int frame = 0;
    while (s_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!s_running) break;

        // Fill SBS pattern into staging
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped);
        if (SUCCEEDED(hr)) {
            fill_sbs((uint32_t*)mapped.pData, WIN_W, WIN_H, mapped.RowPitch / 4, frame);
            ctx->Unmap(staging, 0);
        }
        ctx->CopyResource(sbs_tex, staging);

        if (s_lenticular) {
            // Interleaved: SBS texture → lenticular compositor → backbuffer
            bool ok = lc.render(ctx, sbs_srv, lent, quilt, bb_rtv, WIN_W, WIN_H);
            if (frame == 0)
                printf("[FRAME 0] Lenticular: %s\n", ok ? "ON" : "FAILED");
        } else {
            // Raw SBS: just copy texture to backbuffer directly
            ID3D11Texture2D* bb_tex = nullptr;
            swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb_tex);
            if (bb_tex) {
                ctx->CopyResource(bb_tex, sbs_tex);
                bb_tex->Release();
            }
            if (frame == 0)
                printf("[FRAME 0] Raw SBS (no interleaving)\n");
        }

        swap->Present(1, 0);
        frame++;

        if (frame % 300 == 0) {
            printf("[FRAME %d] Running...\n", frame);
        }
    }

    printf("\nShutting down...\n");
    lc.shutdown();
    sbs_srv->Release();
    bb_rtv->Release();
    staging->Release();
    sbs_tex->Release();
    swap->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);
    printf("Done. %d frames.\n", frame);
    return 0;
}
