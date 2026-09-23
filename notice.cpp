// On-screen notices for Wasteland Storms.
//
// Build-agnostic on purpose: instead of game-internal render functions (which
// differ between the GOG and Steam executables), it hooks the DirectX 11
// swap chain's Present. The address of Present is read from the vtable of a
// throwaway device + swap chain created once, on the game thread, the first
// time the game ticks (never from DllMain, where creating a D3D device is not
// safe). Nothing is drawn -- and ImGui is not even started -- unless a notice
// is on screen, so the steady-state cost is one timestamp check per frame.

#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <cctype>
#include "MinHook.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include <vector>
#include <cstdint>
#pragma comment(lib, "d3d11.lib")

void LogLine(const char* fmt, ...);

// gamefont.cpp
struct GameGlyph { float u0, v0, u1, v1; int w, h, yoff; };
struct GameFont {
    bool ok = false;
    int atlasW = 0, atlasH = 0, lineHeight = 0, firstChar = 0;
    float advanceAdjust = -4.0f;
    std::vector<uint8_t> alpha;
    std::vector<GameGlyph> glyphs;
    std::vector<uint16_t> map;
};
bool LoadGameFontFile(std::vector<uint8_t>& font);
bool ParseGameFont(const std::vector<uint8_t>& f, GameFont& out);

// The game's own UI font, read from the player's archives at startup and
// uploaded as a texture on the first frame that shows a notice. When it
// can't be read the ImGui built-in font is used instead.
static GameFont g_font;
static ID3D11ShaderResourceView* g_fontSrv = nullptr;
static bool g_fontUploadTried = false;

static void UploadGameFont(ID3D11Device* dev) {
    g_fontUploadTried = true;
    if (!g_font.ok) return;
    std::vector<uint32_t> rgba((size_t)g_font.atlasW * g_font.atlasH);
    // The atlas is a distance field: drawing it as opacity puts a soft halo
    // around every letter (it looked bold in game). Cut it at the glyph edge
    // (0.5) with a narrow smoothstep for anti-aliasing, and bake that in.
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) {
        float x = ((float)v / 255.0f - 0.44f) / 0.12f;
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        lut[v] = (uint8_t)(255.0f * x * x * (3.0f - 2.0f * x));
    }
    for (size_t i = 0; i < rgba.size(); i++) rgba[i] = 0x00FFFFFFu | ((uint32_t)lut[g_font.alpha[i]] << 24);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_font.atlasW; td.Height = g_font.atlasH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { rgba.data(), (UINT)(g_font.atlasW * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex)) || !tex) { LogLine("font: texture upload failed"); return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = td.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex, &vd, &g_fontSrv))) g_fontSrv = nullptr;
    tex->Release();
    g_font.alpha.clear(); g_font.alpha.shrink_to_fit();
    LogLine("font: game font %s", g_fontSrv ? "ready" : "view creation failed");
}

static const GameGlyph* GlyphFor(unsigned char c) {
    int i = (int)c - g_font.firstChar;
    if (i < 0 || i >= (int)g_font.map.size()) i = '?' - g_font.firstChar;
    int gi = g_font.map[i];
    return gi < (int)g_font.glyphs.size() ? &g_font.glyphs[gi] : nullptr;
}

static float GameTextWidth(const char* t, float scale) {
    float w = 0;
    for (; *t; t++) { const GameGlyph* g = GlyphFor((unsigned char)*t); if (g) w += (g->w + g_font.advanceAdjust) * scale; }
    return w;
}

static void GameText(ImDrawList* dl, float x, float y, float scale, ImU32 col, const char* t) {
    for (; *t; t++) {
        const GameGlyph* g = GlyphFor((unsigned char)*t);
        if (!g) continue;
        ImVec2 p0(x, y + g->yoff * scale), p1(x + g->w * scale, y + (g->yoff + g->h) * scale);
        dl->AddImage((ImTextureID)g_fontSrv, p0, p1, ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v1), col);
        x += (g->w + g_font.advanceAdjust) * scale;
    }
}

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain* sc, UINT sync, UINT flags);
static PresentFn Present_orig = nullptr;

static CRITICAL_SECTION g_noticeLock;
static bool g_lockReady = false;
static char g_title[128] = "";
static char g_detail[192] = "";
static ULONGLONG g_noticeUntil = 0;
static ULONGLONG g_noticeStart = 0;

static bool g_imguiReady = false, g_imguiFailed = false;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static char g_noticeStatus[96] = "not started";

static bool InitImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) || !g_device) return false;
    g_device->GetImmediateContext(&g_context);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    if (!ImGui_ImplDX11_Init(g_device, g_context)) return false;
    return true;
}

static void DrawNotice(IDXGISwapChain* sc, ULONGLONG now) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)) || !backBuffer) return;
    D3D11_TEXTURE2D_DESC desc; backBuffer->GetDesc(&desc);
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    if (FAILED(hr) || !rtv) return;

    char title[128], detail[192];
    EnterCriticalSection(&g_noticeLock);
    strcpy_s(title, g_title); strcpy_s(detail, g_detail);
    ULONGLONG start = g_noticeStart, until = g_noticeUntil;
    LeaveCriticalSection(&g_noticeLock);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)desc.Width, (float)desc.Height);
    io.DeltaTime = 1.0f / 60.0f;
    io.FontGlobalScale = (float)desc.Height / 1080.0f * 1.7f;
    if (io.FontGlobalScale < 1.0f) io.FontGlobalScale = 1.0f;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // fade in over 0.25 s, out over the last 0.6 s
    float alpha = 1.0f;
    float sinceStart = (float)(now - start) / 1000.0f, toEnd = (float)(until - now) / 1000.0f;
    if (sinceStart < 0.25f) alpha = sinceStart / 0.25f;
    if (toEnd < 0.6f) alpha = toEnd / 0.6f;
    if (alpha < 0.0f) alpha = 0.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!g_fontUploadTried) UploadGameFont(g_device);
    if (g_fontSrv) {
        // Game font: title in capitals like the game's own HUD headers.
        char upper[128]; size_t n = strlen(title); if (n > sizeof(upper) - 1) n = sizeof(upper) - 1;
        for (size_t i = 0; i < n; i++) upper[i] = (char)toupper((unsigned char)title[i]);
        upper[n] = 0;
        float unit = (float)desc.Height / 1080.0f;
        float sTitle = 0.62f * unit, sDetail = 0.48f * unit;
        float wT = GameTextWidth(upper, sTitle), wD = detail[0] ? GameTextWidth(detail, sDetail) : 0.0f;
        float lineT = g_font.lineHeight * sTitle, lineD = g_font.lineHeight * sDetail;
        // Plain white letters, no background (user preference, 2026-09-23).
        float y = io.DisplaySize.y * 0.085f;
        float cx = io.DisplaySize.x * 0.5f;
        GameText(dl, cx - wT * 0.5f, y, sTitle, IM_COL32(255, 255, 255, (int)(255 * alpha)), upper);
        if (detail[0])
            GameText(dl, cx - wD * 0.5f, y + lineT, sDetail, IM_COL32(255, 255, 255, (int)(235 * alpha)), detail);
        ImGui::Render();
        ID3D11RenderTargetView* prevRtv = nullptr; ID3D11DepthStencilView* prevDsv = nullptr;
        g_context->OMGetRenderTargets(1, &prevRtv, &prevDsv);
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
        if (prevRtv) prevRtv->Release();
        if (prevDsv) prevDsv->Release();
        rtv->Release();
        return;
    }
    ImFont* font = ImGui::GetFont();
    float sizeTitle = ImGui::GetFontSize() * 1.25f, sizeDetail = ImGui::GetFontSize();
    ImVec2 tTitle = font->CalcTextSizeA(sizeTitle, FLT_MAX, 0.0f, title);
    ImVec2 tDetail = detail[0] ? font->CalcTextSizeA(sizeDetail, FLT_MAX, 0.0f, detail) : ImVec2(0, 0);
    float pad = sizeDetail * 0.8f;
    float w = (tTitle.x > tDetail.x ? tTitle.x : tDetail.x) + pad * 2.0f;
    float h = tTitle.y + (detail[0] ? tDetail.y + pad * 0.4f : 0.0f) + pad * 2.0f;
    float x = (io.DisplaySize.x - w) * 0.5f, y = io.DisplaySize.y * 0.09f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(20, 14, 8, (int)(200 * alpha)), pad * 0.5f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(214, 140, 52, (int)(220 * alpha)), pad * 0.5f, 0, 2.0f);
    dl->AddText(font, sizeTitle, ImVec2(x + (w - tTitle.x) * 0.5f, y + pad), IM_COL32(240, 200, 140, (int)(255 * alpha)), title);
    if (detail[0])
        dl->AddText(font, sizeDetail, ImVec2(x + (w - tDetail.x) * 0.5f, y + pad + tTitle.y + pad * 0.4f), IM_COL32(225, 215, 200, (int)(235 * alpha)), detail);

    ImGui::Render();

    ID3D11RenderTargetView* prevRtv = nullptr; ID3D11DepthStencilView* prevDsv = nullptr;
    g_context->OMGetRenderTargets(1, &prevRtv, &prevDsv);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    rtv->Release();
}

static HRESULT STDMETHODCALLTYPE Present_hook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ULONGLONG now = GetTickCount64();
    if (now < g_noticeUntil && !g_imguiFailed) {
        if (!g_imguiReady) {
            g_imguiReady = InitImGui(sc);
            if (!g_imguiReady) { g_imguiFailed = true; LogLine("notice: could not start the overlay renderer"); }
            else LogLine("notice: overlay renderer ready");
        }
        if (g_imguiReady) DrawNotice(sc, now);
    }
    return Present_orig(sc, sync, flags);
}

// Throwaway device + swap chain on a hidden window, only to read Present's
// address from the IDXGISwapChain vtable (slot 8).
static void* FindPresentAddress() {
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WastelandStormsDummy";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return nullptr;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    void* present = nullptr;
    if (SUCCEEDED(hr) && sc) present = (*(void***)sc)[8];
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return present;
}

// Call once from the game thread (first game tick).
void Notice_Init() {
    if (!g_lockReady) { InitializeCriticalSection(&g_noticeLock); g_lockReady = true; }
    std::vector<uint8_t> file;
    if (LoadGameFontFile(file) && !ParseGameFont(file, g_font)) LogLine("font: game font file not understood, using the built-in font");
    void* present = FindPresentAddress();
    if (!present) { snprintf(g_noticeStatus, sizeof(g_noticeStatus), "Present not found"); LogLine("notice: %s, notices disabled", g_noticeStatus); return; }
    MH_STATUS c = MH_CreateHook(present, (LPVOID)Present_hook, (LPVOID*)&Present_orig);
    MH_STATUS e = (c == MH_OK) ? MH_EnableHook(present) : c;
    snprintf(g_noticeStatus, sizeof(g_noticeStatus), "%s", MH_StatusToString(e));
    LogLine("notice: Present hook %s", g_noticeStatus);
}

void Notice_Show(const char* title, const char* detail, int milliseconds) {
    if (!g_lockReady) return;
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_noticeLock);
    strcpy_s(g_title, title ? title : "");
    strcpy_s(g_detail, detail ? detail : "");
    // keep the fade-in only when nothing was showing
    if (now >= g_noticeUntil) g_noticeStart = now;
    g_noticeUntil = now + (ULONGLONG)milliseconds;
    LeaveCriticalSection(&g_noticeLock);
}
