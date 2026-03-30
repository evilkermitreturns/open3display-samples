// SPDX-License-Identifier: MIT

// ============================================================================
// Open3D Standalone Test — No game, no injection
//
// Creates a D3D11 window, renders a stereo test pattern, submits to
// Open3D DisplayRuntime, shows live SBS/TAB/anaglyph output.
// Proves the entire pipeline end-to-end on real hardware.
//
// Controls:
//   ESC    — exit
//   1-4    — switch mode (SBS, TAB, Anaglyph RC, Anaglyph Dubois)
//   Space  — toggle stereo on/off (ramp test)
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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static bool s_running = true;
static int  s_mode_request = -1;   // -1 = no change
static bool s_toggle_stereo = false;

static int WIN_W = 0;  // Set to monitor resolution at startup
static int WIN_H = 0;
static int TEX_W = 0;
static int TEX_H = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
        s_running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == '1') s_mode_request = 0;  // SBS
        if (wp == '2') s_mode_request = 1;  // TAB
        if (wp == '3') s_mode_request = 2;  // Anaglyph RC
        if (wp == '4') s_mode_request = 3;  // Anaglyph Dubois
        if (wp == '5') s_mode_request = 10; // Test pattern
        if (wp == '6') s_mode_request = 20; // Lenticular
        if (wp == VK_SPACE) s_toggle_stereo = true;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void fill_test_pattern(uint32_t* pixels, int w, int h, int pitch_pixels, int frame_num) {
    int half_w = w / 2;

    // Scale all sizes relative to eye width — looks the same at any resolution
    float scale = half_w / 640.0f;  // 1.0 at 1280 wide, 3.0 at 3840 wide

    // Bouncing foreground box (white, closer to viewer)
    float t = frame_num * 0.03f;
    int box_cx = half_w / 2;
    int box_cy = h / 2;
    int box_x = (int)(sinf(t) * 0.15f * half_w) + box_cx;
    int box_y = (int)(cosf(t * 0.7f) * 0.08f * h) + box_cy;
    int box_size = (int)(0.08f * half_w);  // 8% of eye width

    // Parallax offset — scales with resolution for consistent stereo depth
    int parallax = (int)(0.015f * half_w);  // 1.5% of eye width

    // Background depth box (cyan, further back = more parallax)
    int bg_box_x = (int)(sinf(t * 0.5f) * 0.10f * half_w) + box_cx;
    int bg_box_y = (int)(cosf(t * 0.3f) * 0.06f * h) + box_cy;
    int bg_box_w = (int)(0.12f * half_w);
    int bg_box_h = (int)(0.06f * h);
    int bg_parallax = (int)(0.03f * half_w);  // 3% = deeper

    // Crosshair size
    int cross_len = (int)(0.06f * half_w);  // 6% of eye width
    int cross_thick = (int)(2 * scale);      // 2px at 640, 6px at 1920
    if (cross_thick < 1) cross_thick = 1;

    // Eye label size
    int label_w = (int)(0.05f * half_w);
    int label_h = (int)(0.03f * h);

    for (int y = 0; y < h; y++) {
        uint32_t* row = pixels + y * pitch_pixels;
        for (int x = 0; x < w; x++) {
            bool is_left = (x < half_w);
            int lx = is_left ? x : (x - half_w);

            // Gradient background — left eye red-tinted, right eye blue-tinted
            uint8_t gx = (uint8_t)(lx * 180 / half_w + 40);
            uint8_t gy = (uint8_t)(y * 60 / h + 20);
            uint32_t color;
            if (is_left) {
                color = 0xFF000000 | (gy) | ((gx / 2) << 8) | (gx << 16);
            } else {
                color = 0xFF000000 | (gx) | ((gx / 2) << 8) | (gy << 16);
            }

            // Background depth box (cyan, more parallax = further back)
            int bbx = bg_box_x + (is_left ? -bg_parallax : bg_parallax);
            if (lx >= bbx - bg_box_w && lx < bbx + bg_box_w &&
                y >= bg_box_y - bg_box_h && y < bg_box_y + bg_box_h) {
                color = 0xFF00CCCC;  // Cyan (BGRA)
            }

            // Foreground box (white, less parallax = closer)
            int bx = box_x + (is_left ? -parallax : parallax);
            if (lx >= bx - box_size && lx < bx + box_size &&
                y >= box_y - box_size && y < box_y + box_size) {
                color = 0xFFFFFFFF;
            }

            // Green crosshair at screen plane (zero parallax — same position both eyes)
            int cx = half_w / 2;
            int cy = h / 2;
            if ((abs(lx - cx) <= cross_thick && y > cy - cross_len && y < cy + cross_len) ||
                (abs(y - cy) <= cross_thick && lx > cx - cross_len && lx < cx + cross_len)) {
                color = 0xFF00FF00;
            }

            // Eye label (top-left corner)
            if (y < label_h && lx < label_w) {
                color = is_left ? 0xFFFF8080 : 0xFF8080FF;
            }

            row[x] = color;
        }
    }
}

int main() {
    printf("========================================\n");
    printf("  Open3D Standalone Test\n");
    printf("========================================\n");
    printf("  1 = SBS   2 = TAB   3 = Anaglyph RC\n");
    printf("  4 = Dubois   5 = Test Patterns\n");
    printf("  6 = Lenticular (CubeVi C1 calibration)\n");
    printf("  Space = toggle stereo   ESC = exit\n");
    printf("========================================\n\n");

    // DPI-aware: get true native resolution (bypasses Windows scaling)
    SetProcessDPIAware();

    // MINIMAL TEST: Windowed on primary, no fullscreen, no adapter targeting
    int C1_X = 100, C1_Y = 100;
    bool found_target = false;  // false = windowed mode
    wchar_t target_device_name[64] = {};
    WIN_W = 1280;
    WIN_H = 720;
    fprintf(stderr, "[MINIMAL] Windowed %dx%d at (%d,%d) on primary monitor\n", WIN_W, WIN_H, C1_X, C1_Y);
    TEX_W = WIN_W;
    TEX_H = WIN_H;

    // Create borderless fullscreen window on C1
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Open3DTest";
    RegisterClassExW(&wc);

    DWORD wnd_style = found_target ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowExW(0, L"Open3DTest",
        L"Open3D Test (ESC to exit)",
        wnd_style, C1_X, C1_Y, WIN_W, WIN_H,
        nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    // Create D3D11 device + swap chain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = WIN_W;
    scd.BufferDesc.Height = WIN_H;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate = { 60, 1 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc = { 1, 0 };
    scd.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;

    // Find the DXGI adapter+output that covers our target window position
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    IDXGIAdapter1* target_adapter = nullptr;
    if (factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ai++) {
            IDXGIOutput* output = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
                DXGI_OUTPUT_DESC od;
                output->GetDesc(&od);
                int dxgi_w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
                int dxgi_h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
                fprintf(stderr, "[DXGI] Adapter %u Output %u: '%ls' at (%ld,%ld) %dx%d\n",
                       ai, oi, od.DeviceName,
                       od.DesktopCoordinates.left, od.DesktopCoordinates.top, dxgi_w, dxgi_h);
                // Match target by device name — DXGI adapter needed for D3D device creation
                if (found_target && wcscmp(od.DeviceName, target_device_name) == 0 && !target_adapter) {
                    target_adapter = adapter;
                    target_adapter->AddRef();
                    fprintf(stderr, "[DXGI] -> MATCHED target '%ls'! Adapter %u\n", od.DeviceName, ai);
                }
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }

    fprintf(stderr, "[DEVICE] target_adapter=%s\n", target_adapter ? "found" : "NULL (using default)");
    HRESULT hr = D3D11CreateDeviceAndSwapChain(target_adapter,
        target_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swap, &device, nullptr, &ctx);
    if (target_adapter) target_adapter->Release();

    if (FAILED(hr)) {
        fprintf(stderr, "FATAL: D3D11 device creation failed: 0x%08X\n", (unsigned)hr);
        return 1;
    }
    fprintf(stderr, "[OK] D3D11 device created\n");

    // Find the C1's DXGI output and go exclusive fullscreen on it (skip if windowed debug)
    IDXGIDevice* dxgi_device = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    IDXGIAdapter* dxgi_adapter = nullptr;
    if (dxgi_device) dxgi_device->GetAdapter(&dxgi_adapter);
    IDXGIOutput* c1_output = nullptr;
    if (dxgi_adapter) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; dxgi_adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC od;
            output->GetDesc(&od);
            int ow = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            int oh = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            fprintf(stderr, "[DXGI-FS] Output %u: '%ls' at (%ld,%ld) %dx%d\n",
                   oi, od.DeviceName, od.DesktopCoordinates.left, od.DesktopCoordinates.top, ow, oh);
            // Match target by device name
            if (found_target && wcscmp(od.DeviceName, target_device_name) == 0) {
                c1_output = output;
                c1_output->AddRef();
                fprintf(stderr, "[DXGI-FS] -> MATCHED target for fullscreen!\n");
            }
            output->Release();
        }
        dxgi_adapter->Release();
    }
    if (dxgi_device) dxgi_device->Release();

    if (c1_output && found_target) {
        hr = swap->SetFullscreenState(TRUE, c1_output);
        if (SUCCEEDED(hr)) {
            fprintf(stderr, "[OK] Exclusive fullscreen on C1 at %dx%d\n", WIN_W, WIN_H);
        } else {
            fprintf(stderr, "[WARN] Exclusive fullscreen on C1 failed (0x%08X)\n", (unsigned)hr);
        }
        c1_output->Release();
    } else {
        printf("[WARN] CubeVi C1 output not found — running windowed\n");
    }

    // Create stereo texture (DEFAULT for GPU use)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = TEX_W;
    td.Height = TEX_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* stereo_tex = nullptr;
    hr = device->CreateTexture2D(&td, nullptr, &stereo_tex);
    if (FAILED(hr) || !stereo_tex) {
        printf("FATAL: Failed to create stereo texture (0x%08X)\n", (unsigned)hr);
        ctx->Release();
        device->Release();
        swap->SetFullscreenState(FALSE, nullptr);
        swap->Release();
        DestroyWindow(hwnd);
        return 1;
    }

    // Staging texture for CPU writes
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D* staging = nullptr;
    hr = device->CreateTexture2D(&td, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        printf("FATAL: Failed to create staging texture (0x%08X)\n", (unsigned)hr);
        stereo_tex->Release();
        ctx->Release();
        device->Release();
        swap->SetFullscreenState(FALSE, nullptr);
        swap->Release();
        DestroyWindow(hwnd);
        return 1;
    }

    printf("[OK] Test textures created (%dx%d)\n", TEX_W, TEX_H);

    // Create graphics device abstraction
    stereo::D3D11GraphicsDevice gfx(device, ctx);
    gfx.set_swap_chain(swap);

    // Initialize Open3D runtime
    stereo::DisplayRuntime runtime;
    stereo::RuntimeConfig config;
    config.screen_width_mm = 600.0f;
    config.viewing_distance_mm = 650.0f;
    runtime.set_config(config);

    if (runtime.init(&gfx) != O3D_SUCCESS) {
        printf("FATAL: Open3D runtime init failed\n");
        staging->Release();
        stereo_tex->Release();
        ctx->Release();
        device->Release();
        swap->SetFullscreenState(FALSE, nullptr);
        swap->Release();
        DestroyWindow(hwnd);
        return 1;
    }
    printf("[OK] Open3D runtime initialized\n");

    // SBS mode — hold steady for geometry verification
    O3DResult qr = runtime.select_backend("sbs");
    fprintf(stderr, "[MODE] select_backend('sbs') -> %s\n", stereo::result_name(qr));
    // Auto-cycle disabled for minimal test
    int auto_mode_idx = 0;
    int auto_cycle_frames = 999999;
    printf("[OK] Backend: %s\n", runtime.get_current_backend_id());
    printf("[OK] State: %s\n\n", stereo::state_name(runtime.get_state()));

    // Main loop
    int frame = 0;
    bool stereo_on = true;
    const char* mode_names[] = { "sbs", "tab", "anaglyph_rc", "anaglyph_dubois" };

    while (s_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!s_running) break;

        // Exit after 3000 frames (~18 seconds at 165Hz)
        if (frame > 3000) {
            fprintf(stderr, "[MINIMAL] Test complete, exiting\n");
            s_running = false;
            break;
        }

        // Handle mode switch
        if (s_mode_request >= 0 && s_mode_request < 4) {
            runtime.select_backend(mode_names[s_mode_request]);
            printf("[MODE] Switched to: %s\n", mode_names[s_mode_request]);
            s_mode_request = -1;
        }
        if (s_mode_request == 20) {
            // Configure lenticular with CubeVi C1 calibration from profile DB
            stereo::LenticularConfig lc{};
            lc.interval     = 19.6169f;   // Lens pitch in pixels
            lc.slope        = 0.1021f;    // Subpixel tilt
            lc.x0           = 3.59f;      // Phase offset
            lc.panel_width  = 1440.0f;    // Native panel width
            lc.panel_height = 2560.0f;    // Native panel height
            runtime.set_lenticular_config(lc);

            // 2-view SBS quilt (our SBS scene is already 2 views)
            stereo::QuiltConfig qc{};
            qc.columns    = 2;
            qc.rows       = 1;
            qc.view_count = 2;
            qc.view_width = TEX_W / 2;
            qc.view_height = TEX_H;
            qc.reverse_views = false;  // SBS input, no X-reversal for 2-view
            runtime.set_quilt_config(qc);

            O3DResult qr = runtime.select_backend("quilt");
            printf("[MODE] select_backend('quilt') -> %s\n", stereo::result_name(qr));
            if (qr == O3D_SUCCESS) {
                printf("[MODE] Switched to: Lenticular (CubeVi C1 calibration)\n");
            } else {
                printf("[MODE] FAILED to switch to quilt — falling back to SBS\n");
            }
            printf("       Lens: interval=%.4f slope=%.4f x0=%.2f panel=%dx%d\n",
                   lc.interval, lc.slope, lc.x0, (int)lc.panel_width, (int)lc.panel_height);
            printf("       Quilt: %dx%d, %d views, %dx%d per view\n",
                   qc.columns, qc.rows, qc.view_count, qc.view_width, qc.view_height);
            s_mode_request = -1;
        }
        if (s_mode_request == 10) {
            // Cycle through test patterns — hold for 120 frames so it's visible
            static int tp_idx = 0;
            static int tp_hold = 0;
            if (tp_hold == 0) {
                const char* tp_names[] = { "ConvergenceGrid", "DepthGradient", "LenticularBars", "ColorBars" };
                auto tp = static_cast<stereo::TestPattern>(tp_idx);
                auto result = runtime.render_test_pattern(tp);
                printf("[TEST PATTERN] %s -> %s\n", tp_names[tp_idx], stereo::result_name(result));
                tp_idx = (tp_idx + 1) % 4;
                tp_hold = 120; // Hold for 120 frames (~2 seconds)
            }
            tp_hold--;
            if (tp_hold <= 0) s_mode_request = -1;
            // Skip normal frame submission — let the test pattern show
            runtime.on_present();
            swap->Present(1, 0);
            frame++;
            continue;
        }

        // Handle stereo toggle
        if (s_toggle_stereo) {
            stereo_on = !stereo_on;
            printf("[STEREO] %s\n", stereo_on ? "ON" : "OFF");
            s_toggle_stereo = false;
        }

        if (stereo_on) {
            // Fill test pattern into staging texture
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped);
            if (SUCCEEDED(hr)) {
                int pitch_pixels = mapped.RowPitch / 4;
                fill_test_pattern(
                    static_cast<uint32_t*>(mapped.pData),
                    TEX_W, TEX_H, pitch_pixels, frame);
                ctx->Unmap(staging, 0);
            }

            // Copy to GPU texture
            ctx->CopyResource(stereo_tex, staging);

            // Submit to Open3D
            stereo::EyeRegion left  = { sizeof(stereo::EyeRegion), 0, 0, (uint32_t)(TEX_W / 2), (uint32_t)TEX_H };
            stereo::EyeRegion right = { sizeof(stereo::EyeRegion), (uint32_t)(TEX_W / 2), 0, (uint32_t)(TEX_W / 2), (uint32_t)TEX_H };
            O3DResult result = runtime.submit_frame(stereo_tex, left, right);
            if (result != O3D_SUCCESS && frame < 3) {
                printf("[WARN] submit_frame failed: %d\n", static_cast<int>(result));
            }
        }

        // Present (compositor draws to backbuffer)
        runtime.on_present();
        swap->Present(1, 0);

        // Stats every 120 frames
        if (frame > 0 && frame % 120 == 0) {
            auto stats = runtime.get_stats();
            stereo::FrameTiming ft;
            runtime.get_frame_timing(ft);
            printf("[STATS] Frame %5d | %s | FPS %.1f | Frame %.2fms | Backend %.3fms | GPU %.3fms | Submitted %llu Presented %llu Dropped %llu\n",
                   frame, stereo::state_name(stats.state),
                   stats.actual_fps, stats.last_frame_time_ms,
                   ft.backend_time_ms, stats.compositor_gpu_ms,
                   stats.frames_submitted, stats.frames_presented, stats.frames_dropped);
        }

        frame++;
    }

    // Cleanup
    printf("\nShutting down...\n");
    runtime.shutdown();
    swap->SetFullscreenState(FALSE, nullptr);
    staging->Release();
    stereo_tex->Release();
    swap->Release();
    ctx->Release();
    device->Release();
    DestroyWindow(hwnd);

    printf("Open3D test complete. %d frames rendered.\n", frame);
    return 0;
}
