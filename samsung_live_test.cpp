// SPDX-License-Identifier: MIT
// ============================================================================
// samsung_live_test.cpp — Live 3D Visual Test on Samsung Odyssey 3D
//
// Full DisplayRuntime pipeline + LeiaSR lens activation.
// Runs persistently with animated stereo content.
// This is the PROOF that Open3Display produces real 3D output.
//
// Controls:
//   1 = SBS (side-by-side, for comparison)
//   2 = Lenticular (3D on Samsung lens)
//   3 = Depth reprojection (mono+depth → stereo → lenticular)
//   4 = Anaglyph RC (for non-3D monitor testing)
//   S = Screenshot to render_proof/
//   ESC = Exit (disables lens)
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <direct.h>
#include <new>

#include <spdlog/spdlog.h>

// Open3Display runtime (full pipeline)
#include "display_runtime.h"
#include "display_types.h"
#include "devices/d3d11_device.h"
#include "../src/tracking/simulated_tracking_provider.h"

// LeiaSR SDK — lens activation + calibration
#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/world/display/display.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/weaver/Weaver.h"
#include "sr/weaver/WeaverTypes.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static bool s_running = true;
static int  s_mode_request = -1;
static bool s_screenshot = false;
static float g_eye_sep = 0.008f;
static float g_lent_interval = 5.4f;
static float g_lent_slope = 0.29f;
static float g_lent_x0 = 0.0f;
static int g_subpixel_mode = 1;  // 0=RGB, 1=BGR, 2=RGBW
static int g_mouse_x = 0, g_mouse_y = 0;
static bool g_mouse_down = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
        s_running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == '1') s_mode_request = 1;
        if (wp == '2') s_mode_request = 2;
        if (wp == '3') s_mode_request = 3;
        if (wp == '4') s_mode_request = 4;
        if (wp == 'S') s_screenshot = true;
    }
    if (msg == WM_MOUSEMOVE) { g_mouse_x = LOWORD(lp); g_mouse_y = HIWORD(lp); }
    if (msg == WM_LBUTTONDOWN) { g_mouse_down = true; g_mouse_x = LOWORD(lp); g_mouse_y = HIWORD(lp); }
    if (msg == WM_LBUTTONUP) { g_mouse_down = false; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// Test pattern: SBS stereo with depth cues
// ============================================================================

// Draw a horizontal slider bar into the pixel buffer
static void draw_slider(uint32_t* pixels, int pitch_px, int x0, int y0,
                        int bar_w, int bar_h, float value, float min_val, float max_val,
                        uint32_t bar_color, uint32_t knob_color) {
    float norm = (value - min_val) / (max_val - min_val);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int knob_x = x0 + (int)(norm * bar_w);

    for (int y = y0; y < y0 + bar_h; y++) {
        for (int x = x0; x < x0 + bar_w; x++) {
            pixels[y * pitch_px + x] = bar_color;
        }
    }
    // Knob (wider for easier clicking)
    for (int y = y0 - 4; y < y0 + bar_h + 4; y++) {
        for (int x = knob_x - 6; x < knob_x + 6; x++) {
            if (x >= 0 && y >= 0) pixels[y * pitch_px + x] = knob_color;
        }
    }
}

// Simple 5x7 digit renderer for on-screen values
static void draw_char(uint32_t* pixels, int pitch_px, int cx, int cy, char ch, uint32_t color) {
    // Minimal: just draw a colored block for each character position
    // (real font rendering is overkill for calibration UI)
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 8; x++)
            if (cy+y >= 0 && cx+x >= 0)
                pixels[(cy+y) * pitch_px + cx + x] = color;
}

static void draw_label(uint32_t* pixels, int pitch_px, int x, int y, const char* text, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        // Simple block per char — enough to see labels
        for (int dy = 0; dy < 14; dy++)
            for (int dx = 0; dx < 9; dx++)
                if (y+dy >= 0 && x + i*10 + dx >= 0)
                    pixels[(y+dy) * pitch_px + x + i*10 + dx] = (text[i] != ' ') ? color : 0xFF1A1A2E;
    }
}

// Static 3D scene — simple solid objects at different depths
// Calibration info drawn directly in the frame
static void fill_stereo_scene(uint32_t* pixels, int w, int h, int pitch_px, int frame) {
    int half = w / 2;
    float eye_sep = g_eye_sep;

    // Static objects at fixed depths
    // Screen plane (convergence) = z=0.5
    // Near (pop-out) = z=0.3
    // Far (into screen) = z=0.8

    for (int y = 0; y < h; y++) {
        uint32_t* row = pixels + y * pitch_px;
        for (int x = 0; x < w; x++) {
            bool is_left = (x < half);
            int lx = is_left ? x : (x - half);
            float eye_off = is_left ? -eye_sep : eye_sep;

            // Dark background
            uint32_t color = 0xFF1A1A2E;

            // --- BIG RED CIRCLE at screen plane (z=0.5, zero parallax) ---
            {
                float obj_x = 0.0f, obj_y = 0.0f, obj_z = 0.5f;
                float proj_x = (obj_x - eye_off) / obj_z;
                float proj_y = obj_y / obj_z;
                int cx = (int)((proj_x + 0.5f) * half);
                int cy = (int)((0.5f - proj_y) * h);
                int r = (int)(0.08f * half);
                int dx = lx - cx, dy = y - cy;
                if (dx*dx + dy*dy < r*r)
                    color = 0xFFCC3333;  // Red at screen plane
            }

            // --- BIG WHITE SQUARE floating IN FRONT (z=0.3, negative parallax = pop-out) ---
            {
                float obj_x = -0.08f, obj_y = 0.05f, obj_z = 0.3f;
                float proj_x = (obj_x - eye_off) / obj_z;
                float proj_y = obj_y / obj_z;
                int cx = (int)((proj_x + 0.5f) * half);
                int cy = (int)((0.5f - proj_y) * h);
                int sz = (int)(0.06f * half);
                if (lx >= cx-sz && lx < cx+sz && y >= cy-sz && y < cy+sz)
                    color = 0xFFFFFFFF;  // White pop-out
            }

            // --- BIG CYAN SQUARE far BEHIND screen (z=0.9, positive parallax) ---
            {
                float obj_x = 0.1f, obj_y = -0.04f, obj_z = 0.9f;
                float proj_x = (obj_x - eye_off) / obj_z;
                float proj_y = obj_y / obj_z;
                int cx = (int)((proj_x + 0.5f) * half);
                int cy = (int)((0.5f - proj_y) * h);
                int sz = (int)(0.10f * half);
                if (lx >= cx-sz && lx < cx+sz && y >= cy-sz && y < cy+sz)
                    color = 0xFF00CCCC;  // Cyan behind
            }

            // --- GREEN CROSSHAIR at exact screen plane (zero parallax reference) ---
            {
                int cx = half / 2, cy = h / 2;
                int thick = 3, len = (int)(0.04f * half);
                if ((abs(lx - cx) <= thick && abs(y - cy) < len) ||
                    (abs(y - cy) <= thick && abs(lx - cx) < len))
                    color = 0xFF00FF00;
            }

            row[x] = color;
        }
    }

    // ---- Draw calibration sliders on LEFT side (both eyes see same UI) ----
    // These are drawn into both eye halves at the same position (zero parallax = on screen)
    for (int eye = 0; eye < 2; eye++) {
        int ox = eye * half;
        int sx = 20 + ox;  // Slider start X in this eye

        // Background panel
        for (int y = 30; y < 400; y++)
            for (int x = sx - 10; x < sx + 380; x++)
                if (x >= ox && x < ox + half)
                    pixels[y * pitch_px + x] = 0xC0000000;

        // Slider 1: Interval
        draw_label(pixels, pitch_px, sx, 30, "INTERVAL", 0xFFFFFF00);
        draw_slider(pixels, pitch_px, sx, 50, 350, 8, g_lent_interval, 1.0f, 30.0f, 0xFF444444, 0xFFFFFF00);

        // Slider 2: Slope
        draw_label(pixels, pitch_px, sx, 100, "SLOPE", 0xFF00FFFF);
        draw_slider(pixels, pitch_px, sx, 120, 350, 8, g_lent_slope, -1.0f, 1.0f, 0xFF444444, 0xFF00FFFF);

        // Slider 3: X0
        draw_label(pixels, pitch_px, sx, 170, "PHASE X0", 0xFFFF88FF);
        draw_slider(pixels, pitch_px, sx, 190, 350, 8, g_lent_x0, -20.0f, 20.0f, 0xFF444444, 0xFFFF88FF);

        // Slider 4: Eye separation
        draw_label(pixels, pitch_px, sx, 240, "EYE SEP", 0xFFFF8800);
        draw_slider(pixels, pitch_px, sx, 260, 350, 8, g_eye_sep, 0.001f, 0.03f, 0xFF444444, 0xFFFF8800);

        // Button: Subpixel layout
        const char* sp_names[] = {"RGB", "BGR", "RGBW"};
        draw_label(pixels, pitch_px, sx, 320, sp_names[g_subpixel_mode], 0xFF00FF00);
        draw_label(pixels, pitch_px, sx + 60, 320, "CLICK", 0xFF888888);
    }
}

// ============================================================================
// BMP screenshot
// ============================================================================

static bool save_screenshot(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                            IDXGISwapChain* sc, const char* path) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    D3D11_TEXTURE2D_DESC desc; bb->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &staging);
    ctx->CopyResource(staging, bb);
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);

    FILE* f = fopen(path, "wb");
    if (f) {
        uint32_t row_size = (desc.Width * 3 + 3) & ~3;
        uint32_t data_size = row_size * desc.Height;
        uint8_t hdr[54] = {};
        hdr[0] = 'B'; hdr[1] = 'M';
        *(uint32_t*)(hdr + 2) = 54 + data_size;
        *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
        *(int32_t*)(hdr + 18) = (int32_t)desc.Width;
        *(int32_t*)(hdr + 22) = -(int32_t)desc.Height;
        *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 24;
        *(uint32_t*)(hdr + 34) = data_size;
        fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> row_buf(row_size, 0);
        for (uint32_t y = 0; y < desc.Height; y++) {
            auto* src = (uint8_t*)m.pData + y * m.RowPitch;
            for (uint32_t x = 0; x < desc.Width; x++) {
                row_buf[x*3+0] = src[x*4+0]; row_buf[x*3+1] = src[x*4+1]; row_buf[x*3+2] = src[x*4+2];
            }
            fwrite(row_buf.data(), 1, row_size, f);
        }
        fclose(f);
    }
    ctx->Unmap(staging, 0);
    staging->Release(); bb->Release();
    return f != nullptr;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    printf("==============================================\n");
    printf("  Open3Display Samsung 3D Live Test\n");
    printf("  Full Runtime + LeiaSR Lens Activation\n");
    printf("----------------------------------------------\n");
    printf("  1 = SBS       2 = Lenticular 3D\n");
    printf("  3 = Depth     4 = Anaglyph RC\n");
    printf("  S = Screenshot  ESC = Exit\n");
    printf("==============================================\n\n");

    SetProcessDPIAware();
    _mkdir("render_proof");

    // ---- Find Samsung display via DXGI ----
    int WIN_W = 3840, WIN_H = 2160;
    int target_x = 0, target_y = 0;
    bool found_samsung = false;

    // Enumerate all displays and let user pick (or pass index as arg)
    struct DisplayInfo { int x, y, w, h; UINT adapter_idx; wchar_t name[64]; };
    std::vector<DisplayInfo> displays;

    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    IDXGIAdapter1* target_adapter = nullptr;
    UINT target_adapter_idx = 0;

    if (factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ai++) {
            IDXGIOutput* output = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
                DXGI_OUTPUT_DESC od;
                output->GetDesc(&od);
                int ow = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
                int oh = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
                DisplayInfo di;
                di.x = od.DesktopCoordinates.left; di.y = od.DesktopCoordinates.top;
                di.w = ow; di.h = oh; di.adapter_idx = ai;
                wcsncpy(di.name, od.DeviceName, 63);
                displays.push_back(di);
                output->Release();
            }
            adapter->Release();
        }
    }

    printf("Displays found:\n");
    for (int i = 0; i < (int)displays.size(); i++) {
        printf("  [%d] '%ls' at (%d,%d) %dx%d\n", i,
               displays[i].name, displays[i].x, displays[i].y, displays[i].w, displays[i].h);
    }

    int pick = -1;
    // Auto-detect: env var > command line > LeiaSR SDK detection > fallback
    const char* env_display = getenv("O3D_TARGET_DISPLAY");
    if (env_display) {
        pick = atoi(env_display);
    } else if (argc > 1) {
        pick = atoi(argv[1]);
    }
    // Auto-detect 3D display using our display detector (reads EDID, checks 3D_present flag)
    if (pick < 0) {
        stereo::DetectedDisplay detected[8];
        int count = stereo::DisplayRuntime::enumerate_displays(detected, 8);
        printf("[DETECT] Found %d displays via EDID:\n", count);
        for (int i = 0; i < count; i++) {
            printf("  [%d] '%s' (%s) %dx%d phys=%ux%umm 3D=%s HDR=%s\n",
                   i, detected[i].name, detected[i].id.manufacturer,
                   detected[i].resolution_width, detected[i].resolution_height,
                   detected[i].physical_width_mm, detected[i].physical_height_mm,
                   detected[i].capabilities.stereo_native ? "YES" : "no",
                   detected[i].capabilities.hdr_supported ? "YES" : "no");
            // Match to DXGI display list by desktop coordinates
            if (detected[i].capabilities.stereo_native) {
                for (int d = 0; d < (int)displays.size(); d++) {
                    if (displays[d].x == detected[i].desktop_left &&
                        displays[d].y == detected[i].desktop_top) {
                        pick = d;
                        printf("[DETECT] -> 3D display matched to DXGI index %d\n", d);
                        break;
                    }
                }
            }
        }
        // If EDID 3D flag not set, try matching by Leia SDK
        if (pick < 0) {
            try {
                SR::SRContext probe_ctx;
                SR::Display* sr_disp = SR::Display::create(probe_ctx);
                if (sr_disp) {
                    int sr_w = sr_disp->getResolutionWidth();
                    int sr_h = sr_disp->getResolutionHeight();
                    printf("[LEIA] SR Display reports: %dx%d — matching to DXGI outputs\n", sr_w, sr_h);
                    for (int i = 0; i < (int)displays.size(); i++) {
                        if (displays[i].w == sr_w && displays[i].h == sr_h && pick < 0) {
                            pick = i;
                        }
                    }
                }
            } catch (...) {
                printf("[LEIA] Auto-detect probe failed\n");
            }
        }
    }
    // Samsung 3D is the 60Hz 4K display (Display 1). Primary is 165Hz.
    if (pick < 0) pick = 1;
    if (pick >= 0 && pick < (int)displays.size()) {
        found_samsung = true;
        target_x = displays[pick].x;
        target_y = displays[pick].y;
        WIN_W = displays[pick].w;
        WIN_H = displays[pick].h;
        target_adapter_idx = displays[pick].adapter_idx;
        printf("[TARGET] Display %d: '%ls' at (%d,%d) %dx%d\n",
               pick, displays[pick].name, target_x, target_y, WIN_W, WIN_H);

        // Get the adapter for this display
        IDXGIAdapter1* adapter = nullptr;
        if (factory && SUCCEEDED(factory->EnumAdapters1(target_adapter_idx, &adapter))) {
            target_adapter = adapter;  // Already AddRef'd by EnumAdapters1
        }
    }

    if (!found_samsung) {
        printf("[WARN] No display selected — using primary %dx%d\n", WIN_W, WIN_H);
        WIN_W = GetSystemMetrics(SM_CXSCREEN);
        WIN_H = GetSystemMetrics(SM_CYSCREEN);
    }

    // ---- LeiaSR SDK: lens activation + calibration ----
    SR::SRContext* sr_ctx = nullptr;
    SR::SwitchableLensHint* lens_hint = nullptr;

    stereo::LenticularConfig lent{};
    lent.panel_width  = (float)WIN_W;
    lent.panel_height = (float)WIN_H;
    // Samsung Odyssey 3D defaults — will be overridden by Leia SDK if available
    lent.interval = 6.0f;     // ~2px lens pitch * 3 subpixels (Samsung 2-view)
    lent.slope    = 0.29f;    // Slant from SDK
    lent.x0       = 0.0f;
    // Samsung IPS panels commonly use BGR subpixel order
    lent.subpixel_layout = O3D_SUBPIXEL_BGR;

    // ---- D3D11 setup ----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SamsungLiveTest";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"SamsungLiveTest",
        L"Open3Display Samsung 3D Live Test",
        WS_POPUP, target_x, target_y, WIN_W, WIN_H,
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

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        target_adapter, target_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swap, &device, nullptr, &ctx);
    if (target_adapter) target_adapter->Release();
    if (factory) factory->Release();

    if (FAILED(hr)) {
        printf("FATAL: D3D11 device creation failed: 0x%08X\n", (unsigned)hr);
        return 1;
    }
    printf("[OK] D3D11 device created\n");

    // ---- Extract Samsung calibration via LeiaSR SDK ----
    try {
        sr_ctx = new(std::nothrow) SR::SRContext();
        if (sr_ctx) {
            lens_hint = SR::SwitchableLensHint::create(*sr_ctx);

            // Create temp weaver to trigger calibration data load
            SR::IDX11Weaver1* temp_weaver = nullptr;
            WeaverErrorCode wrc = SR::CreateDX11Weaver(sr_ctx, ctx, hwnd, &temp_weaver);
            if (wrc == WeaverSuccess && temp_weaver) {
                float slant = Dimenco::Weaver::GetSlant();
                float pitch = Dimenco::Weaver::GetPx();
                printf("[LEIA] Calibration: slant=%.6f pitch=%.4fpx\n", slant, pitch);

                float n_val = Dimenco::Weaver::GetN();
                float d_n   = Dimenco::Weaver::GetDoN();
                printf("[LEIA] Raw SDK values: pitch=%.6f slant=%.6f N=%.6f DoN=%.6f\n",
                       pitch, slant, n_val, d_n);

                if (pitch > 0.0f && std::isfinite(pitch) && std::isfinite(slant)) {
                    // Leia SDK GetPx() returns lens pitch in pixels.
                    // Our shader converts pixel position to subpixel position using _SubpixelCount.
                    // So interval must be in subpixels: pitch_px * subpixels_per_pixel.
                    // For BGR (3 subpixels): interval = pitch * 3
                    // For RGBW (4 subpixels): interval = pitch * 4
                    float spc = (lent.subpixel_layout == O3D_SUBPIXEL_RGBW) ? 4.0f : 3.0f;
                    lent.interval = pitch * spc;
                    lent.slope    = slant;
                    lent.x0       = 0.0f;
                    printf("[LEIA] Samsung calibration: pitch=%.4fpx * %.0f = interval=%.4f subpx, slope=%.6f, layout=%s\n",
                           pitch, spc, lent.interval, lent.slope,
                           lent.subpixel_layout == O3D_SUBPIXEL_BGR ? "BGR" : "RGB");
                }
                temp_weaver->destroy();
            } else {
                printf("[LEIA] Weaver creation failed (code %d) — using fallback calibration\n", (int)wrc);
            }

            sr_ctx->initialize();
            if (lens_hint) {
                lens_hint->enable();
                printf("[LEIA] LENS ENABLED — you should see 3D!\n");
            }
        }
    } catch (const std::exception& e) {
        printf("[LEIA] SDK error: %s — continuing without lens\n", e.what());
    } catch (...) {
        printf("[LEIA] SDK error (unknown) — continuing without lens\n");
    }

    // Sync globals for slider display
    g_lent_interval = lent.interval;
    g_lent_slope = lent.slope;
    g_lent_x0 = lent.x0;
    g_subpixel_mode = static_cast<int>(lent.subpixel_layout);

    printf("[CONFIG] Lenticular: interval=%.4f slope=%.6f x0=%.2f panel=%dx%d layout=%s\n",
           lent.interval, lent.slope, lent.x0, WIN_W, WIN_H,
           lent.subpixel_layout == O3D_SUBPIXEL_BGR ? "BGR" : "RGB");

    // ---- Textures ----
    int TEX_W = WIN_W, TEX_H = WIN_H;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = TEX_W; td.Height = TEX_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* stereo_tex = nullptr;
    device->CreateTexture2D(&td, nullptr, &stereo_tex);

    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&td, nullptr, &staging);

    // Depth texture for depth reprojection scene
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = TEX_W; dd.Height = TEX_H; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R32_FLOAT; dd.SampleDesc = {1, 0};
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* depth_tex = nullptr;
    device->CreateTexture2D(&dd, nullptr, &depth_tex);

    // Fill depth with gradient (reversed-Z: center near=1.0, edges far=0.1)
    dd.Usage = D3D11_USAGE_STAGING; dd.BindFlags = 0; dd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* depth_staging = nullptr;
    device->CreateTexture2D(&dd, nullptr, &depth_staging);
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(depth_staging, 0, D3D11_MAP_WRITE, 0, &m))) {
            for (int y = 0; y < TEX_H; y++) {
                float* row = (float*)((uint8_t*)m.pData + y * m.RowPitch);
                for (int x = 0; x < TEX_W; x++) {
                    // Radial depth: near at center (1.0), far at edges (0.1)
                    float fx = (float)(x - TEX_W / 2) / (TEX_W / 2);
                    float fy = (float)(y - TEX_H / 2) / (TEX_H / 2);
                    float dist = sqrtf(fx * fx + fy * fy);
                    row[x] = 1.0f - dist * 0.9f;  // 1.0 at center, 0.1 at corners
                    if (row[x] < 0.05f) row[x] = 0.05f;
                }
            }
            ctx->Unmap(depth_staging, 0);
        }
        ctx->CopyResource(depth_tex, depth_staging);
    }

    // ---- Initialize Open3Display Runtime ----
    stereo::D3D11GraphicsDevice gfx(device, ctx);
    gfx.set_swap_chain(swap);

    stereo::DisplayRuntime runtime;
    stereo::RuntimeConfig cfg;
    cfg.screen_width_mm = 600.0f;     // Samsung 27" ≈ 597mm
    cfg.viewing_distance_mm = 650.0f;  // Arm's length
    cfg.comfort_preset = O3D_COMFORT_STANDARD;
    runtime.set_config(cfg);

    // Head tracking — SimulatedTrackingProvider for now (slow gentle sway)
    // This feeds viewer position into the lenticular phase offset for sweet spot following.
    // Replace with Leia's face tracker for real head tracking later.
    stereo::SimulatedTrackingConfig tcfg;
    tcfg.pattern = stereo::SimulatedPattern::Lissajous;
    tcfg.frequency_hz = 0.15f;       // Very slow — natural head drift speed
    tcfg.amplitude_x_mm = 30.0f;     // ±30mm lateral (realistic head movement)
    tcfg.amplitude_y_mm = 10.0f;     // ±10mm vertical
    stereo::SimulatedTrackingProvider tracker(tcfg);
    runtime.set_tracking_provider(&tracker);
    runtime.set_tracking_mode(O3D_TRACKING_RUNTIME);

    if (runtime.init(&gfx) != O3D_SUCCESS) {
        printf("FATAL: Open3D runtime init failed\n");
        return 1;
    }

    // Configure lenticular — 2-view for Samsung
    runtime.set_lenticular_config(lent);
    stereo::QuiltConfig quilt{};
    quilt.columns = 2; quilt.rows = 1; quilt.view_count = 2;
    quilt.view_width = TEX_W / 2; quilt.view_height = TEX_H;
    quilt.view_cone_deg = 40.0f;  // Samsung 2-view: ~40° viewing cone
    quilt.reverse_views = false;  // 2-view SBS: no X-reversal
    runtime.set_quilt_config(quilt);

    // SBS mode — lenticular routing kicks in automatically when interval > 0
    runtime.select_backend("sbs");
    int current_scene = 1;
    float eye_sep = 0.008f;  // Adjustable with +/- keys
    printf("[OK] Runtime initialized in lenticular mode\n");
    printf("  CALIBRATION CONTROLS:\n");
    printf("  UP/DOWN    = interval (lens pitch)\n");
    printf("  LEFT/RIGHT = x0 (phase offset)\n");
    printf("  PgUp/PgDn  = slope (lens tilt)\n");
    printf("  +/-        = eye separation (3D strength)\n");
    printf("  1-4        = switch mode\n\n");

    stereo::EyeRegion left_eye  = {sizeof(stereo::EyeRegion), 0, 0, (uint32_t)(TEX_W / 2), (uint32_t)TEX_H};
    stereo::EyeRegion right_eye = {sizeof(stereo::EyeRegion), (uint32_t)(TEX_W / 2), 0, (uint32_t)(TEX_W / 2), (uint32_t)TEX_H};

    // ---- Main Loop ----
    int frame = 0;
    int screenshot_counter = 0;

    while (s_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!s_running) break;

        // Mouse slider calibration — sliders on left edge of frame
        // Each slider: y range maps to a value range. Click+drag adjusts.
        if (g_mouse_down && g_mouse_x < 400) {
            bool changed = false;
            float norm = (float)g_mouse_x / 400.0f;  // 0..1 within slider area

            // Slider regions (by Y position in pixels)
            if (g_mouse_y >= 40 && g_mouse_y < 100) {
                // Interval: 1.0 .. 30.0
                lent.interval = 1.0f + norm * 29.0f;
                changed = true;
            } else if (g_mouse_y >= 110 && g_mouse_y < 170) {
                // Slope: -1.0 .. 1.0
                lent.slope = -1.0f + norm * 2.0f;
                changed = true;
            } else if (g_mouse_y >= 180 && g_mouse_y < 240) {
                // X0: -20.0 .. 20.0
                lent.x0 = -20.0f + norm * 40.0f;
                changed = true;
            } else if (g_mouse_y >= 250 && g_mouse_y < 310) {
                // Eye sep: 0.001 .. 0.03
                eye_sep = 0.001f + norm * 0.029f;
                g_eye_sep = eye_sep;
                changed = true;
            } else if (g_mouse_y >= 320 && g_mouse_y < 380) {
                // Subpixel layout: click cycles RGB→BGR→RGBW
                static int last_click_frame = -100;
                if (frame - last_click_frame > 30) {  // Debounce
                    g_subpixel_mode = (g_subpixel_mode + 1) % 3;
                    lent.subpixel_layout = static_cast<O3DSubpixelLayout>(g_subpixel_mode);
                    last_click_frame = frame;
                    changed = true;
                }
            }
            if (changed) {
                g_lent_interval = lent.interval;
                g_lent_slope = lent.slope;
                g_lent_x0 = lent.x0;
                runtime.set_lenticular_config(lent);
                printf("[CAL] interval=%.2f slope=%.4f x0=%.2f eye=%.4f layout=%s\n",
                       lent.interval, lent.slope, lent.x0, eye_sep,
                       g_subpixel_mode == 1 ? "BGR" : g_subpixel_mode == 2 ? "RGBW" : "RGB");
            }
        }

        // Handle mode switches
        if (s_mode_request > 0) {
            current_scene = s_mode_request;
            switch (current_scene) {
                case 1: runtime.select_backend("sbs"); printf("[MODE] SBS\n"); break;
                case 2: runtime.select_backend("quilt"); printf("[MODE] Lenticular 3D\n"); break;
                case 3: runtime.select_backend("quilt"); printf("[MODE] Depth Reprojection → Lenticular\n"); break;
                case 4: runtime.select_backend("anaglyph_rc"); printf("[MODE] Anaglyph RC\n"); break;
            }
            s_mode_request = -1;
        }

        if (current_scene == 3) {
            // Depth reprojection: submit mono color + depth, runtime generates stereo
            // Use the stereo_tex as color (fill with solid gradient for visibility)
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &m))) {
                uint32_t* pixels = (uint32_t*)m.pData;
                int pitch_px = m.RowPitch / 4;
                for (int y = 0; y < TEX_H; y++) {
                    for (int x = 0; x < TEX_W; x++) {
                        // Colorful gradient (no stereo separation — depth shader adds it)
                        uint8_t r = (uint8_t)(x * 255 / TEX_W);
                        uint8_t g = (uint8_t)(y * 255 / TEX_H);
                        uint8_t b = (uint8_t)(128 + (int)(sinf(frame * 0.05f + x * 0.01f) * 60));
                        pixels[y * pitch_px + x] = 0xFF000000 | (b << 16) | (g << 8) | r;
                    }
                }
                ctx->Unmap(staging, 0);
            }
            ctx->CopyResource(stereo_tex, staging);

            stereo::DepthFrameInfo dfi;
            dfi.color_texture = stereo_tex;
            dfi.depth_texture = depth_tex;
            dfi.near_plane = 0.1f;
            dfi.far_plane = 1000.0f;
            dfi.reversed_z = O3D_TRUE;
            dfi.depth_strength = 1.0f;
            dfi.convergence_distance = 5.0f;
            dfi.M00 = 1.0f;
            dfi.view_count = 2;
            runtime.submit_depth_frame(dfi);
        } else {
            // Scene 1 (SBS) or Scene 4 (Anaglyph): submit as stereo frame
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &m))) {
                fill_stereo_scene((uint32_t*)m.pData, TEX_W, TEX_H, m.RowPitch / 4, frame);
                ctx->Unmap(staging, 0);
            }
            ctx->CopyResource(stereo_tex, staging);

            runtime.submit_frame(stereo_tex, left_eye, right_eye);
        }

        runtime.on_present();
        swap->Present(1, 0);

        // Screenshot
        if (s_screenshot) {
            char path[256];
            snprintf(path, sizeof(path), "render_proof/samsung_%03d.bmp", screenshot_counter++);
            if (save_screenshot(device, ctx, swap, path))
                printf("[SCREENSHOT] Saved: %s\n", path);
            s_screenshot = false;
        }

        // Stats every 300 frames (~5 seconds at 60Hz)
        if (frame > 0 && frame % 300 == 0) {
            auto stats = runtime.get_stats();
            printf("[STATS] Frame %d | %s | FPS %.1f | GPU %.3fms | Submitted %llu Presented %llu\n",
                   frame, stereo::state_name(stats.state),
                   stats.actual_fps, stats.compositor_gpu_ms,
                   stats.frames_submitted, stats.frames_presented);
        }

        frame++;
    }

    // ---- Cleanup ----
    printf("\nShutting down...\n");
    runtime.set_tracking_provider(nullptr);  // Detach before tracker destructor
    runtime.shutdown();

    if (lens_hint) {
        lens_hint->disable();
        printf("[LEIA] Lens disabled\n");
    }
    if (sr_ctx) {
        delete sr_ctx;
        printf("[LEIA] SR context destroyed\n");
    }

    depth_staging->Release();
    depth_tex->Release();
    staging->Release();
    stereo_tex->Release();
    swap->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    printf("Samsung live test complete. %d frames.\n", frame);
    return 0;
}
