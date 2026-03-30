// SPDX-License-Identifier: MIT
// ============================================================================
// lenticular_calibrator.cpp — Professional Lenticular Calibration Tool
//
// Two windows:
//   1. CONTROL PANEL on primary monitor — Win32 native sliders + display picker
//   2. TEST PATTERN on target display — clean red/blue through lenticular lens
//
// Real Win32 trackbar controls, proper labels, display auto-detection.
// This is a shipping tool — other users will use this to calibrate their displays.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <direct.h>
#include <new>

#include <spdlog/spdlog.h>
#include "backends/weaving_compositor_d3d11.h"
#include "stereo_frame.h"
#include "display_types.h"

#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/weaver/Weaver.h"
#include "sr/weaver/WeaverTypes.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ============================================================================
// Display info
// ============================================================================
struct DisplayInfo {
    int x, y, w, h;
    std::string label;
};
static std::vector<DisplayInfo> g_displays;

// ============================================================================
// Calibration state
// ============================================================================
static float g_interval = 5.4f;
static float g_slope    = 0.29f;
static float g_x0       = 0.0f;
static int   g_layout   = 1;   // 0=RGB, 1=BGR, 2=RGBW
static int   g_pattern  = 0;   // 0=red/blue, 1=white/black, 2=gradient
static int   g_target   = -1;  // Target display index

// ============================================================================
// Win32 control IDs
// ============================================================================
enum {
    ID_SLIDER_INTERVAL = 100,
    ID_SLIDER_SLOPE,
    ID_SLIDER_X0,
    ID_COMBO_DISPLAY,
    ID_COMBO_LAYOUT,
    ID_COMBO_PATTERN,
    ID_BTN_SAVE,
    ID_BTN_LAUNCH,
    ID_LABEL_INTERVAL,
    ID_LABEL_SLOPE,
    ID_LABEL_X0,
    ID_LABEL_STATUS,
};

static HWND g_ctrl_wnd = nullptr;
static HWND g_slider_interval, g_slider_slope, g_slider_x0;
static HWND g_label_interval, g_label_slope, g_label_x0, g_label_status;
static HWND g_combo_display, g_combo_layout, g_combo_pattern;

// ============================================================================
// Render state
// ============================================================================
static bool g_rendering = false;
static HWND g_render_wnd = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11Texture2D* g_sbs_tex = nullptr;
static ID3D11Texture2D* g_staging = nullptr;
static ID3D11ShaderResourceView* g_sbs_srv = nullptr;
static ID3D11RenderTargetView* g_bb_rtv = nullptr;
static stereo::LenticularCompositorD3D11 g_lc;
static SR::SRContext* g_sr_ctx = nullptr;
static SR::SwitchableLensHint* g_lens = nullptr;
static int g_render_w = 0, g_render_h = 0;

static void update_value_labels() {
    char buf[64];
    snprintf(buf, 64, "Interval: %.3f", g_interval);
    SetWindowTextA(g_label_interval, buf);
    snprintf(buf, 64, "Slope: %.4f", g_slope);
    SetWindowTextA(g_label_slope, buf);
    snprintf(buf, 64, "Phase X0: %.2f", g_x0);
    SetWindowTextA(g_label_x0, buf);
}

static void cleanup_render() {
    if (g_lens) { g_lens->disable(); g_lens = nullptr; }
    g_lc.shutdown();
    if (g_sbs_srv) g_sbs_srv->Release();
    if (g_bb_rtv) g_bb_rtv->Release();
    if (g_staging) g_staging->Release();
    if (g_sbs_tex) g_sbs_tex->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    if (g_sr_ctx) { delete g_sr_ctx; g_sr_ctx = nullptr; }
    if (g_render_wnd) { DestroyWindow(g_render_wnd); g_render_wnd = nullptr; }
    g_sbs_srv = nullptr; g_bb_rtv = nullptr; g_staging = nullptr;
    g_sbs_tex = nullptr; g_swap = nullptr; g_ctx = nullptr; g_dev = nullptr;
    g_rendering = false;
}

static bool start_render(int disp_idx) {
    cleanup_render();
    if (disp_idx < 0 || disp_idx >= (int)g_displays.size()) return false;

    auto& d = g_displays[disp_idx];
    g_render_w = d.w; g_render_h = d.h;
    g_target = disp_idx;

    // Render window — borderless on target display
    WNDCLASSEXW rwc = {}; rwc.cbSize = sizeof(rwc);
    rwc.lpfnWndProc = DefWindowProcW; rwc.hInstance = GetModuleHandleW(nullptr);
    rwc.lpszClassName = L"CalRender"; rwc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&rwc);
    g_render_wnd = CreateWindowExW(WS_EX_TOPMOST, L"CalRender", L"", WS_POPUP,
        d.x, d.y, d.w, d.h, nullptr, nullptr, rwc.hInstance, nullptr);
    ShowWindow(g_render_wnd, SW_SHOW);
    SetForegroundWindow(g_ctrl_wnd);  // Keep control panel focused

    // D3D11
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1; scd.BufferDesc.Width = d.w; scd.BufferDesc.Height = d.h;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_render_wnd; scd.SampleDesc = {1,0}; scd.Windowed = TRUE;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &g_swap, &g_dev, nullptr, &g_ctx))) return false;

    // LeiaSR
    try {
        g_sr_ctx = new(std::nothrow) SR::SRContext();
        if (g_sr_ctx) {
            g_lens = SR::SwitchableLensHint::create(*g_sr_ctx);
            SR::IDX11Weaver1* tw = nullptr;
            if (SR::CreateDX11Weaver(g_sr_ctx, g_ctx, g_render_wnd, &tw) == WeaverSuccess && tw) {
                float pitch = Dimenco::Weaver::GetPx();
                float slant = Dimenco::Weaver::GetSlant();
                if (pitch > 0 && std::isfinite(pitch)) {
                    g_interval = pitch * 3.0f;
                    g_slope = slant;
                    // Update sliders to match SDK values
                    SendMessage(g_slider_interval, TBM_SETPOS, TRUE, (LPARAM)(g_interval * 100));
                    SendMessage(g_slider_slope, TBM_SETPOS, TRUE, (LPARAM)((g_slope + 1.0f) * 5000));
                    update_value_labels();
                }
                tw->destroy();
            }
            g_sr_ctx->initialize();
            if (g_lens) g_lens->enable();
        }
    } catch (...) {}

    // Textures
    D3D11_TEXTURE2D_DESC td = {}; td.Width = d.w; td.Height = d.h;
    td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1,0}; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, nullptr, &g_sbs_tex);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateTexture2D(&td, nullptr, &g_staging);

    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {}; svd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_sbs_tex, &svd, &g_sbs_srv);

    ID3D11Texture2D* bb = nullptr; g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dev->CreateRenderTargetView(bb, nullptr, &g_bb_rtv); bb->Release();

    if (!g_lc.init(g_dev)) return false;
    g_rendering = true;

    SetWindowTextA(g_label_status, "RENDERING — close one eye to check separation");
    return true;
}

static void render_frame() {
    if (!g_rendering) return;
    int W = g_render_w, H = g_render_h, half = W / 2;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_staging, 0, D3D11_MAP_WRITE, 0, &m))) {
        uint32_t* px = (uint32_t*)m.pData;
        int pitch = m.RowPitch / 4;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                bool left = (x < half);
                if (g_pattern == 0)
                    px[y*pitch+x] = left ? 0xFFFF0000 : 0xFF0000FF;
                else if (g_pattern == 1)
                    px[y*pitch+x] = left ? 0xFFFFFFFF : 0xFF000000;
                else {
                    int lx = left ? x : (x-half);
                    uint8_t v = (uint8_t)(lx * 255 / (half>1?half:1));
                    px[y*pitch+x] = left ? (0xFF000000|(v<<16)) : (0xFF000000|v);
                }
            }
        g_ctx->Unmap(g_staging, 0);
    }
    g_ctx->CopyResource(g_sbs_tex, g_staging);

    stereo::LenticularConfig lent{};
    lent.interval = g_interval; lent.slope = g_slope; lent.x0 = g_x0;
    lent.panel_width = (float)W; lent.panel_height = (float)H;
    lent.subpixel_layout = static_cast<stereo::O3DSubpixelLayout>(g_layout);

    stereo::QuiltConfig quilt{}; quilt.columns = 2; quilt.rows = 1; quilt.view_count = 2;
    quilt.view_width = W/2; quilt.view_height = H; quilt.reverse_views = false;

    g_lc.render(g_ctx, g_sbs_srv, lent, quilt, g_bb_rtv, W, H);
    g_swap->Present(1, 0);
}

static void save_calibration() {
    _mkdir("data");
    const char* sp[] = {"RGB","BGR","RGBW"};
    FILE* f = fopen("data/samsung_calibration.json", "w");
    if (f) {
        fprintf(f, "{\n  \"display\": \"Calibrated Display\",\n"
                   "  \"interval\": %.6f,\n  \"slope\": %.6f,\n  \"x0\": %.6f,\n"
                   "  \"subpixel_layout\": \"%s\",\n  \"view_count\": 2,\n"
                   "  \"panel_width\": %d,\n  \"panel_height\": %d\n}\n",
                g_interval, g_slope, g_x0, sp[g_layout], g_render_w, g_render_h);
        fclose(f);
        SetWindowTextA(g_label_status, "SAVED to data/samsung_calibration.json");
    }
}

// ============================================================================
// Control panel window procedure
// ============================================================================
static LRESULT CALLBACK CtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HSCROLL: {
        // Slider changed
        HWND slider = (HWND)lp;
        if (slider == g_slider_interval) {
            int pos = (int)SendMessage(slider, TBM_GETPOS, 0, 0);
            g_interval = pos / 100.0f;
        } else if (slider == g_slider_slope) {
            int pos = (int)SendMessage(slider, TBM_GETPOS, 0, 0);
            g_slope = (pos / 5000.0f) - 1.0f;
        } else if (slider == g_slider_x0) {
            int pos = (int)SendMessage(slider, TBM_GETPOS, 0, 0);
            g_x0 = (pos / 250.0f) - 20.0f;
        }
        update_value_labels();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_COMBO_LAYOUT && HIWORD(wp) == CBN_SELCHANGE)
            g_layout = (int)SendMessage(g_combo_layout, CB_GETCURSEL, 0, 0);
        else if (LOWORD(wp) == ID_COMBO_PATTERN && HIWORD(wp) == CBN_SELCHANGE)
            g_pattern = (int)SendMessage(g_combo_pattern, CB_GETCURSEL, 0, 0);
        else if (LOWORD(wp) == ID_COMBO_DISPLAY && HIWORD(wp) == CBN_SELCHANGE) {
            int sel = (int)SendMessage(g_combo_display, CB_GETCURSEL, 0, 0);
            start_render(sel);
        }
        else if (LOWORD(wp) == ID_BTN_SAVE) save_calibration();
        else if (LOWORD(wp) == ID_BTN_LAUNCH) {
            int sel = (int)SendMessage(g_combo_display, CB_GETCURSEL, 0, 0);
            start_render(sel);
        }
        return 0;
    case WM_DESTROY:
        cleanup_render();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_label(HWND parent, const char* text, int x, int y, int w, int h, int id = 0) {
    return CreateWindowA("STATIC", text, WS_CHILD|WS_VISIBLE, x, y, w, h, parent, (HMENU)(intptr_t)id, nullptr, nullptr);
}

static HWND make_slider(HWND parent, int x, int y, int w, int h, int id, int lo, int hi, int val) {
    HWND s = CreateWindowA(TRACKBAR_CLASSA, "", WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
        x, y, w, h, parent, (HMENU)(intptr_t)id, nullptr, nullptr);
    SendMessage(s, TBM_SETRANGEMIN, FALSE, lo);
    SendMessage(s, TBM_SETRANGEMAX, FALSE, hi);
    SendMessage(s, TBM_SETPOS, TRUE, val);
    return s;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    InitCommonControls();
    SetProcessDPIAware();
    _mkdir("data");

    // Enumerate displays
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (factory) {
        IDXGIAdapter1* a = nullptr;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ai++) {
            IDXGIOutput* o = nullptr;
            for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; oi++) {
                DXGI_OUTPUT_DESC d; o->GetDesc(&d);
                DisplayInfo di;
                di.x = d.DesktopCoordinates.left; di.y = d.DesktopCoordinates.top;
                di.w = d.DesktopCoordinates.right - di.x; di.h = d.DesktopCoordinates.bottom - di.y;
                char buf[128];
                snprintf(buf, 128, "[%d] %dx%d at (%d,%d)", (int)g_displays.size(), di.w, di.h, di.x, di.y);
                di.label = buf;
                g_displays.push_back(di);
                o->Release();
            }
            a->Release();
        }
        factory->Release();
    }

    // Create control panel window on primary monitor
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CtrlProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CalCtrl"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_ctrl_wnd = CreateWindowExW(0, L"CalCtrl", L"Open3Display Lenticular Calibrator",
        WS_OVERLAPPEDWINDOW, 100, 100, 520, 520,
        nullptr, nullptr, wc.hInstance, nullptr);

    int y = 10, lw = 490;

    // Title
    make_label(g_ctrl_wnd, "LENTICULAR CALIBRATOR — close one eye to check", 10, y, lw, 20);
    y += 30;

    // Display selector
    make_label(g_ctrl_wnd, "Target Display:", 10, y, 120, 20);
    g_combo_display = CreateWindowA("COMBOBOX", "", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        130, y-2, 350, 200, g_ctrl_wnd, (HMENU)ID_COMBO_DISPLAY, nullptr, nullptr);
    for (auto& d : g_displays)
        SendMessageA(g_combo_display, CB_ADDSTRING, 0, (LPARAM)d.label.c_str());
    if (!g_displays.empty()) SendMessage(g_combo_display, CB_SETCURSEL, 0, 0);
    y += 32;

    // Launch button
    CreateWindowA("BUTTON", "Start Calibration", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        10, y, 200, 30, g_ctrl_wnd, (HMENU)ID_BTN_LAUNCH, nullptr, nullptr);
    y += 40;

    // Interval slider
    g_label_interval = make_label(g_ctrl_wnd, "Interval: 5.400", 10, y, lw, 20, ID_LABEL_INTERVAL);
    y += 22;
    g_slider_interval = make_slider(g_ctrl_wnd, 10, y, lw, 30, ID_SLIDER_INTERVAL,
        100, 3000, (int)(g_interval * 100));  // Range: 1.00 to 30.00
    y += 38;

    // Slope slider
    g_label_slope = make_label(g_ctrl_wnd, "Slope: 0.2900", 10, y, lw, 20, ID_LABEL_SLOPE);
    y += 22;
    g_slider_slope = make_slider(g_ctrl_wnd, 10, y, lw, 30, ID_SLIDER_SLOPE,
        0, 10000, (int)((g_slope + 1.0f) * 5000));  // Range: -1.0 to 1.0
    y += 38;

    // X0 slider
    g_label_x0 = make_label(g_ctrl_wnd, "Phase X0: 0.00", 10, y, lw, 20, ID_LABEL_X0);
    y += 22;
    g_slider_x0 = make_slider(g_ctrl_wnd, 10, y, lw, 30, ID_SLIDER_X0,
        0, 10000, 5000);  // Range: -20.0 to 20.0
    y += 38;

    // Layout combo
    make_label(g_ctrl_wnd, "Subpixel Layout:", 10, y, 120, 20);
    g_combo_layout = CreateWindowA("COMBOBOX", "", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
        130, y-2, 120, 100, g_ctrl_wnd, (HMENU)ID_COMBO_LAYOUT, nullptr, nullptr);
    SendMessageA(g_combo_layout, CB_ADDSTRING, 0, (LPARAM)"RGB");
    SendMessageA(g_combo_layout, CB_ADDSTRING, 0, (LPARAM)"BGR");
    SendMessageA(g_combo_layout, CB_ADDSTRING, 0, (LPARAM)"RGBW");
    SendMessage(g_combo_layout, CB_SETCURSEL, g_layout, 0);

    // Pattern combo
    make_label(g_ctrl_wnd, "Pattern:", 270, y, 60, 20);
    g_combo_pattern = CreateWindowA("COMBOBOX", "", WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
        340, y-2, 140, 100, g_ctrl_wnd, (HMENU)ID_COMBO_PATTERN, nullptr, nullptr);
    SendMessageA(g_combo_pattern, CB_ADDSTRING, 0, (LPARAM)"Red / Blue");
    SendMessageA(g_combo_pattern, CB_ADDSTRING, 0, (LPARAM)"White / Black");
    SendMessageA(g_combo_pattern, CB_ADDSTRING, 0, (LPARAM)"Gradient");
    SendMessage(g_combo_pattern, CB_SETCURSEL, 0, 0);
    y += 38;

    // Save button
    CreateWindowA("BUTTON", "Save Calibration", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        10, y, 200, 30, g_ctrl_wnd, (HMENU)ID_BTN_SAVE, nullptr, nullptr);
    y += 40;

    // Status
    g_label_status = make_label(g_ctrl_wnd, "Select a display and click Start Calibration", 10, y, lw, 20, ID_LABEL_STATUS);

    ShowWindow(g_ctrl_wnd, SW_SHOW);
    update_value_labels();

    // Message loop with render
    MSG msg;
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        render_frame();
        if (!g_rendering) Sleep(16);  // Don't spin when not rendering
    }
done:
    cleanup_render();
    return 0;
}
