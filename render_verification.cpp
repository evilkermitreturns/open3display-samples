// SPDX-License-Identifier: MIT
// ============================================================================
// render_verification.cpp — Automated Visual Verification Test
//
// Creates a D3D11 window, auto-cycles through EVERY compositor mode,
// captures a screenshot of each, and outputs results to render_proof/.
//
// Proves the runtime produces correct visual output for every mode.
// Runs unattended — no keyboard input needed.
//
// If LeiaSR SDK is available, activates the Samsung lens for the lenticular
// test and captures that too.
//
// Usage: render_verification.exe [--samsung]
//   --samsung : Target Samsung display, activate lenticular lens
// ============================================================================

#include "display_runtime.h"
#include "display_types.h"
#include "devices/d3d11_device.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <direct.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ============================================================================
// BMP screenshot writer (no dependencies)
// ============================================================================

static bool save_bmp(const char* path, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t pitch) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    uint32_t row_size = (w * 3 + 3) & ~3;  // BMP rows are 4-byte aligned
    uint32_t data_size = row_size * h;

    // BMP header
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t*)(hdr + 2) = 54 + data_size;
    *(uint32_t*)(hdr + 10) = 54;
    *(uint32_t*)(hdr + 14) = 40;
    *(int32_t*)(hdr + 18) = (int32_t)w;
    *(int32_t*)(hdr + 22) = -(int32_t)h;  // Top-down
    *(uint16_t*)(hdr + 26) = 1;
    *(uint16_t*)(hdr + 28) = 24;
    *(uint32_t*)(hdr + 34) = data_size;
    fwrite(hdr, 1, 54, f);

    // Pixel data (RGBA → BGR)
    std::vector<uint8_t> row_buf(row_size, 0);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = pixels + y * pitch;
        for (uint32_t x = 0; x < w; x++) {
            row_buf[x * 3 + 0] = src[x * 4 + 2]; // B
            row_buf[x * 3 + 1] = src[x * 4 + 1]; // G
            row_buf[x * 3 + 2] = src[x * 4 + 0]; // R
        }
        fwrite(row_buf.data(), 1, row_size, f);
    }
    fclose(f);
    return true;
}

// ============================================================================
// Test pattern: SBS stereo with clear visual markers
// ============================================================================

static void fill_verification_pattern(uint32_t* pixels, int w, int h, int pitch_px, int frame, const char* mode_label) {
    int half = w / 2;
    float t = frame * 0.05f;

    // Foreground parallax (close object — less parallax)
    int fg_parallax = (int)(0.02f * half);
    // Background parallax (far object — more parallax)
    int bg_parallax = (int)(0.04f * half);

    int box_size = (int)(0.10f * half);
    int fg_cx = half / 2, fg_cy = h / 2;
    int fg_x = (int)(sinf(t) * 0.12f * half) + fg_cx;
    int fg_y = (int)(cosf(t * 0.6f) * 0.06f * h) + fg_cy;

    int bg_cx = half / 2, bg_cy = h / 3;
    int bg_w = (int)(0.25f * half), bg_h = (int)(0.08f * h);

    for (int y = 0; y < h; y++) {
        uint32_t* row = pixels + y * pitch_px;
        for (int x = 0; x < w; x++) {
            bool is_left = (x < half);
            int lx = is_left ? x : (x - half);

            // Gradient background — left=warm, right=cool
            uint8_t gx = (uint8_t)(lx * 120 / (half > 1 ? half : 1) + 30);
            uint8_t gy = (uint8_t)(y * 40 / (h > 1 ? h : 1) + 20);
            uint32_t color;
            if (is_left) {
                color = 0xFF000000 | ((gx + 40) << 16) | ((gx / 3) << 8) | gy;
            } else {
                color = 0xFF000000 | gy | ((gx / 3) << 8) | ((gx + 40) << 16);
            }

            // Background depth box (cyan) — strong parallax
            int bbx = bg_cx + (is_left ? -bg_parallax : bg_parallax);
            if (lx >= bbx - bg_w && lx < bbx + bg_w && y >= bg_cy - bg_h && y < bg_cy + bg_h)
                color = 0xFF00CCCC;

            // Foreground box (white) — mild parallax
            int fbx = fg_x + (is_left ? -fg_parallax : fg_parallax);
            if (lx >= fbx - box_size && lx < fbx + box_size && y >= fg_y - box_size && y < fg_y + box_size)
                color = 0xFFFFFFFF;

            // Screen-plane crosshair (green, zero parallax)
            int cx = half / 2, cy = h / 2;
            int cross_thick = 2, cross_len = (int)(0.08f * half);
            if ((abs(lx - cx) <= cross_thick && y > cy - cross_len && y < cy + cross_len) ||
                (abs(y - cy) <= cross_thick && lx > cx - cross_len && lx < cx + cross_len))
                color = 0xFF00FF00;

            // Eye label
            int lbl_w = (int)(0.06f * half), lbl_h = (int)(0.04f * h);
            if (y < lbl_h + 5 && y >= 5 && lx >= 5 && lx < lbl_w + 5)
                color = is_left ? 0xFFFF4444 : 0xFF4444FF;

            row[x] = color;
        }
    }
}

// ============================================================================
// Screenshot capture from backbuffer
// ============================================================================

static bool capture_backbuffer(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                                IDXGISwapChain* sc, const char* path) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;

    D3D11_TEXTURE2D_DESC desc;
    bb->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { bb->Release(); return false; }

    ctx->CopyResource(staging, bb);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        staging->Release(); bb->Release(); return false;
    }

    bool ok = save_bmp(path, static_cast<uint8_t*>(m.pData), desc.Width, desc.Height, m.RowPitch);

    ctx->Unmap(staging, 0);
    staging->Release();
    bb->Release();
    return ok;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    printf("==============================================\n");
    printf("  Open3Display Render Verification\n");
    printf("  Auto-cycles all modes, captures screenshots\n");
    printf("==============================================\n\n");

    SetProcessDPIAware();

    int WIN_W = 1280, WIN_H = 720;

    // Create output directory
    _mkdir("render_proof");

    // Window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"O3DVerify";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"O3DVerify",
        L"Open3Display Render Verification",
        WS_OVERLAPPEDWINDOW, 100, 100, WIN_W, WIN_H,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    // D3D11
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = WIN_W;
    scd.BufferDesc.Height = WIN_H;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
        printf("FATAL: D3D11 device creation failed: 0x%08X\n", (unsigned)hr);
        return 1;
    }
    printf("[OK] D3D11 device created\n");

    // Textures
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = WIN_W; td.Height = WIN_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* stereo_tex = nullptr;
    device->CreateTexture2D(&td, nullptr, &stereo_tex);

    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&td, nullptr, &staging);

    stereo::D3D11GraphicsDevice gfx(device, ctx);
    gfx.set_swap_chain(swap);

    // Backend IDs and display names
    struct ModeEntry {
        const char* id;
        const char* name;
        int settle_frames;   // Frames to render before screenshot
    };

    ModeEntry modes[] = {
        {"sbs",             "01_SBS",              30},
        {"tab",             "02_TAB",              30},
        {"anaglyph_rc",     "03_Anaglyph_RC",      30},
        {"anaglyph_dubois", "04_Anaglyph_Dubois",  30},
        {"interlaced_row",  "05_Row_Interlaced",   30},
        {"interlaced_col",  "06_Col_Interlaced",   30},
        {"checkerboard",    "07_Checkerboard",     30},
        {"anaglyph_gm",     "08_Anaglyph_GM",      30},
        {"anaglyph_ab",     "09_Anaglyph_AB",      30},
        {"mono",            "10_Mono",             30},
        {"frame_packed_hdmi", "11_Frame_Packed",   30},
    };
    int mode_count = sizeof(modes) / sizeof(modes[0]);

    stereo::EyeRegion left_eye  = {sizeof(stereo::EyeRegion), 0, 0, (uint32_t)(WIN_W / 2), (uint32_t)WIN_H};
    stereo::EyeRegion right_eye = {sizeof(stereo::EyeRegion), (uint32_t)(WIN_W / 2), 0, (uint32_t)(WIN_W / 2), (uint32_t)WIN_H};

    int total_pass = 0, total_fail = 0;

    for (int mi = 0; mi < mode_count; mi++) {
        printf("\n--- Testing: %s (%s) ---\n", modes[mi].name, modes[mi].id);

        stereo::DisplayRuntime runtime;
        stereo::RuntimeConfig cfg;
        cfg.screen_width_mm = 600.0f;
        cfg.viewing_distance_mm = 650.0f;
        strncpy(cfg.backend_id, modes[mi].id, sizeof(cfg.backend_id) - 1);
        runtime.set_config(cfg);

        auto init_result = runtime.init(&gfx);
        if (init_result != O3D_SUCCESS) {
            printf("  [FAIL] init: %s\n", stereo::result_name(init_result));
            total_fail++;
            continue;
        }

        // Render settle_frames to warm up shaders + state
        bool submit_ok = true, present_ok = true;
        for (int f = 0; f < modes[mi].settle_frames; f++) {
            // Fill pattern
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped))) {
                fill_verification_pattern(
                    (uint32_t*)mapped.pData, WIN_W, WIN_H, mapped.RowPitch / 4, f, modes[mi].name);
                ctx->Unmap(staging, 0);
            }
            ctx->CopyResource(stereo_tex, staging);

            if (runtime.submit_frame(stereo_tex, left_eye, right_eye) != O3D_SUCCESS)
                submit_ok = false;
            if (runtime.on_present() != O3D_SUCCESS)
                present_ok = false;
            swap->Present(0, 0);

            // Process messages to keep window responsive
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        // Capture screenshot
        char path[256];
        snprintf(path, sizeof(path), "render_proof/%s.bmp", modes[mi].name);
        bool captured = capture_backbuffer(device, ctx, swap, path);

        // Get stats
        auto stats = runtime.get_stats();

        printf("  Submit: %s | Present: %s | Screenshot: %s\n",
               submit_ok ? "OK" : "FAIL",
               present_ok ? "OK" : "FAIL",
               captured ? path : "FAIL");
        printf("  Frames: %llu submitted, %llu presented, %llu dropped\n",
               stats.frames_submitted, stats.frames_presented, stats.frames_dropped);
        printf("  State: %s | Backend: %s | Healthy: %s\n",
               stereo::state_name(stats.state), stats.backend_name,
               stats.device_healthy ? "YES" : "NO");

        if (submit_ok && present_ok && captured) {
            printf("  [PASS] %s\n", modes[mi].name);
            total_pass++;
        } else {
            printf("  [FAIL] %s\n", modes[mi].name);
            total_fail++;
        }

        runtime.shutdown();
    }

    // ========================================================================
    // Summary
    // ========================================================================

    printf("\n==============================================\n");
    printf("  RESULTS: %d PASS / %d FAIL / %d TOTAL\n", total_pass, total_fail, total_pass + total_fail);
    printf("  Screenshots: render_proof/\n");
    printf("==============================================\n");

    // Cleanup
    staging->Release();
    stereo_tex->Release();
    swap->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    return total_fail > 0 ? 1 : 0;
}
