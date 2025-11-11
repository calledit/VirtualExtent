#include "pch.h"
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>
#include <Windows.h>
#undef min
#undef max

#include "controllers.h"
#include "d3d.h"     // d3d_device, d3d_context, d3d_xr_projection, d3d_compile_shader
#include "openxr.h"  // xr_input
#include "desktop_plane.h"

using namespace DirectX;

// ---------- Module state ----------

// Per-draw constants
struct ControllerCB {
    XMFLOAT4X4 world;
    XMFLOAT4X4 viewproj;
    XMFLOAT4   color;   // rgba
};

static ID3D11VertexShader* s_vs = nullptr;
static ID3D11PixelShader* s_ps = nullptr;
static ID3D11InputLayout* s_il = nullptr;
static ID3D11Buffer* s_cb = nullptr;

// A small cube as the controller body (pos.xyz, norm.xyz interleaved)
static ID3D11Buffer* s_vbCube = nullptr;
static ID3D11Buffer* s_ibCube = nullptr;

// A thin box for the pointer ray (unit length along -Z, scaled at draw)
static ID3D11Buffer* s_vbRay = nullptr;
static ID3D11Buffer* s_ibRay = nullptr;

static constexpr char kHLSL[] = R"_(
cbuffer ControllerCB : register(b0) {
    float4x4 world;
    float4x4 viewproj;
    float4   color;
};
struct vsIn {
    float3 pos  : POSITION;
    float3 norm : NORMAL;
};
struct psIn {
    float4 pos   : SV_POSITION;
    float3 normW : NORMAL0;
};
psIn vs(vsIn i) {
    psIn o;
    float4 wp = mul(float4(i.pos,1), world);
    o.pos   = mul(wp, viewproj);
    // transform normal
    float3 n = mul(float4(i.norm,0), world).xyz;
    o.normW = normalize(n);
    return o;
}
float4 ps(psIn i) : SV_TARGET {
    // simple lambert with head-light-ish up
    float3 L = normalize(float3(0.3,0.9,0.2));
    float NdotL = saturate(dot(i.normW, L));
    float3 c = color.rgb * (0.25 + 0.75*NdotL);
    return float4(c, color.a);
}
)_";

static constexpr char kFlatColorPS_HLSL[] = R"_(
cbuffer ControllerCB : register(b0) {
    float4x4 world;
    float4x4 viewproj;
    float4   color;
};
float4 ps() : SV_TARGET { return color; }
)_";

static ID3D11PixelShader* s_psFlat = nullptr;

// ---------- Helpers ----------
static void MakeUnitCube(ID3D11Buffer** outVB, ID3D11Buffer** outIB) {
    struct V { float px, py, pz, nx, ny, nz; };
    // 24 unique vertices (cube with normals)
    const V verts[] = {
        // +X
        {+0.5f,-0.5f,-0.5f,  +1,0,0}, {+0.5f,-0.5f,+0.5f, +1,0,0},
        {+0.5f,+0.5f,+0.5f,  +1,0,0}, {+0.5f,+0.5f,-0.5f, +1,0,0},
        // -X
        {-0.5f,-0.5f,+0.5f,  -1,0,0}, {-0.5f,-0.5f,-0.5f,-1,0,0},
        {-0.5f,+0.5f,-0.5f,  -1,0,0}, {-0.5f,+0.5f,+0.5f, -1,0,0},
        // +Y
        {-0.5f,+0.5f,-0.5f,  0,+1,0}, {+0.5f,+0.5f,-0.5f, 0,+1,0},
        {+0.5f,+0.5f,+0.5f,  0,+1,0}, {-0.5f,+0.5f,+0.5f, 0,+1,0},
        // -Y
        {-0.5f,-0.5f,+0.5f,  0,-1,0}, {+0.5f,-0.5f,+0.5f,  0,-1,0},
        {+0.5f,-0.5f,-0.5f,  0,-1,0}, {-0.5f,-0.5f,-0.5f,  0,-1,0},
        // +Z (back)
        {+0.5f,-0.5f,+0.5f,  0,0,+1}, {-0.5f,-0.5f,+0.5f, 0,0,+1},
        {-0.5f,+0.5f,+0.5f,  0,0,+1}, {+0.5f,+0.5f,+0.5f, 0,0,+1},
        // -Z (front)
        {-0.5f,-0.5f,-0.5f,  0,0,-1}, {+0.5f,-0.5f,-0.5f, 0,0,-1},
        {+0.5f,+0.5f,-0.5f,  0,0,-1}, {-0.5f,+0.5f,-0.5f, 0,0,-1},
    };
    const uint16_t inds[] = {
        0,1,2, 0,2,3,   4,5,6, 4,6,7,
        8,9,10, 8,10,11, 12,13,14, 12,14,15,
        16,17,18, 16,18,19, 20,21,22, 20,22,23
    };

    D3D11_BUFFER_DESC vbd = CD3D11_BUFFER_DESC(sizeof(verts), D3D11_BIND_VERTEX_BUFFER);
    D3D11_BUFFER_DESC ibd = CD3D11_BUFFER_DESC(sizeof(inds), D3D11_BIND_INDEX_BUFFER);
    D3D11_SUBRESOURCE_DATA vinit{ verts };
    D3D11_SUBRESOURCE_DATA iinit{ inds };
    d3d_device->CreateBuffer(&vbd, &vinit, outVB);
    d3d_device->CreateBuffer(&ibd, &iinit, outIB);
}

static void MakeUnitRayBox(ID3D11Buffer** outVB, ID3D11Buffer** outIB) {
    // A thin unit-length box along -Z (0 at origin to length=1 towards -Z).
    // We’ll scale to (thickness, thickness, lengthMeters) at draw.
    struct V { float px, py, pz, nx, ny, nz; };
    const float z0 = 0.0f, z1 = -1.0f;
    const V verts[] = {
        // +X
        {+0.5f,-0.5f,z0, +1,0,0},{+0.5f,-0.5f,z1, +1,0,0},{+0.5f,+0.5f,z1, +1,0,0},{+0.5f,+0.5f,z0, +1,0,0},
        // -X
        {-0.5f,-0.5f,z1, -1,0,0},{-0.5f,-0.5f,z0, -1,0,0},{-0.5f,+0.5f,z0, -1,0,0},{-0.5f,+0.5f,z1, -1,0,0},
        // +Y
        {-0.5f,+0.5f,z0, 0,+1,0},{+0.5f,+0.5f,z0, 0,+1,0},{+0.5f,+0.5f,z1, 0,+1,0},{-0.5f,+0.5f,z1, 0,+1,0},
        // -Y
        {-0.5f,-0.5f,z1, 0,-1,0},{+0.5f,-0.5f,z1, 0,-1,0},{+0.5f,-0.5f,z0, 0,-1,0},{-0.5f,-0.5f,z0, 0,-1,0},
        // +Z (near)
        {+0.5f,-0.5f,z0, 0,0,+1},{-0.5f,-0.5f,z0, 0,0,+1},{-0.5f,+0.5f,z0, 0,0,+1},{+0.5f,+0.5f,z0, 0,0,+1},
        // -Z (far)
        {-0.5f,-0.5f,z1, 0,0,-1},{+0.5f,-0.5f,z1, 0,0,-1},{+0.5f,+0.5f,z1, 0,0,-1},{-0.5f,+0.5f,z1, 0,0,-1},
    };
    const uint16_t inds[] = {
        0,1,2, 0,2,3,   4,5,6, 4,6,7,
        8,9,10, 8,10,11, 12,13,14, 12,14,15,
        16,17,18, 16,18,19, 20,21,22, 20,22,23
    };
    D3D11_BUFFER_DESC vbd = CD3D11_BUFFER_DESC(sizeof(verts), D3D11_BIND_VERTEX_BUFFER);
    D3D11_BUFFER_DESC ibd = CD3D11_BUFFER_DESC(sizeof(inds), D3D11_BIND_INDEX_BUFFER);
    D3D11_SUBRESOURCE_DATA vinit{ verts };
    D3D11_SUBRESOURCE_DATA iinit{ inds };
    d3d_device->CreateBuffer(&vbd, &vinit, outVB);
    d3d_device->CreateBuffer(&ibd, &iinit, outIB);
}

// ---------- API ----------

bool Controllers_Init() {
    // Shaders
    ID3DBlob* vsb = d3d_compile_shader(kHLSL, "vs", "vs_5_0");
    ID3DBlob* psb = d3d_compile_shader(kHLSL, "ps", "ps_5_0");
    if (!vsb || !psb) return false;

    d3d_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_vs);
    d3d_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ps);

    // flat-color pixel shader for markers
    ID3DBlob* psbFlat = d3d_compile_shader(kFlatColorPS_HLSL, "ps", "ps_5_0");
    if (psbFlat) {
        d3d_device->CreatePixelShader(psbFlat->GetBufferPointer(), psbFlat->GetBufferSize(), nullptr, &s_psFlat);
        psbFlat->Release();
    }

    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    d3d_device->CreateInputLayout(il, (UINT)_countof(il), vsb->GetBufferPointer(), vsb->GetBufferSize(), &s_il);
    vsb->Release(); psb->Release();

    // Constant buffer
    D3D11_BUFFER_DESC cbd = CD3D11_BUFFER_DESC(sizeof(ControllerCB), D3D11_BIND_CONSTANT_BUFFER);
    d3d_device->CreateBuffer(&cbd, nullptr, &s_cb);

    // Geometry
    MakeUnitCube(&s_vbCube, &s_ibCube);
    MakeUnitRayBox(&s_vbRay, &s_ibRay);

    return true;
}

static void DrawOne(const XrCompositionLayerProjectionView& view, int handIdx, const XMFLOAT4& color) {
    if (!xr_input.renderHand[handIdx]) return;

    // Per-eye view/proj
    XMMATRIX proj = d3d_xr_projection(view.fov, 0.05f, 100.0f);
    XMMATRIX viewM = XMMatrixInverse(nullptr, XMMatrixAffineTransformation(
        DirectX::g_XMOne, DirectX::g_XMZero,
        XMLoadFloat4((XMFLOAT4*)&view.pose.orientation),
        XMLoadFloat3((XMFLOAT3*)&view.pose.position)));

    // Grip pose
    XMVECTOR q = XMLoadFloat4((XMFLOAT4*)&xr_input.handPose[handIdx].orientation);
    XMVECTOR p = XMLoadFloat3((XMFLOAT3*)&xr_input.handPose[handIdx].position);

    // --- Draw controller body (small cube) ---
    {
        float s = 0.05f; // 5 cm cube
        XMMATRIX world =
            XMMatrixScaling(s, s, s) *
            XMMatrixRotationQuaternion(q) *
            XMMatrixTranslationFromVector(p);

        ControllerCB cb{};
        XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
        XMStoreFloat4x4(&cb.viewproj, XMMatrixTranspose(viewM * proj));
        cb.color = color;

        d3d_context->UpdateSubresource(s_cb, 0, nullptr, &cb, 0, 0);
        d3d_context->VSSetConstantBuffers(0, 1, &s_cb);
        d3d_context->PSSetConstantBuffers(0, 1, &s_cb);
        d3d_context->VSSetShader(s_vs, nullptr, 0);
        d3d_context->PSSetShader(s_ps, nullptr, 0);

        UINT stride = sizeof(float) * 6, offset = 0;
        d3d_context->IASetVertexBuffers(0, 1, &s_vbCube, &stride, &offset);
        d3d_context->IASetIndexBuffer(s_ibCube, DXGI_FORMAT_R16_UINT, 0);
        d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3d_context->IASetInputLayout(s_il);
        d3d_context->DrawIndexed(36, 0, 0);
    }

    // --- Compute tilted ray, raycast against desktop, get dynamic length ---
    const float tiltDeg = -35.0f; // natural-feel downward tilt
    XMVECTOR tiltQ = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(tiltDeg));
    XMVECTOR qTilt = XMQuaternionMultiply(tiltQ, q);

    // Unit direction in world for the ray (-Z in local, rotated by qTilt)
    XMVECTOR dirW = XMVector3Normalize(XMVector3Rotate(XMVectorSet(0, 0, -1, 0), qTilt));

    // Raycast first to know if/where we hit
    DirectX::XMFLOAT3 hitW;
    DirectX::XMFLOAT2 uv;
    bool hit = DesktopPlane_Raycast(p, dirW, &hitW, &uv);

    // Choose ray length: to hit if present, else default 3m
    float length = 3.0f;
    if (hit) {
        XMVECTOR h = XMLoadFloat3(&hitW);
        float t = XMVectorGetX(XMVector3Dot(XMVectorSubtract(h, p), dirW)); // distance along dirW
        if (t > 0.0f) length = t;
    }

    // --- Draw pointer ray (stops at intersection) ---
    {
        float thickness = 0.005f; // 5 mm
        XMMATRIX worldRay =
            XMMatrixScaling(thickness, thickness, length) *
            XMMatrixRotationQuaternion(qTilt) *
            XMMatrixTranslationFromVector(p);

        ControllerCB cb{};
        XMStoreFloat4x4(&cb.world, XMMatrixTranspose(worldRay));
        XMStoreFloat4x4(&cb.viewproj, XMMatrixTranspose(viewM * proj));
        cb.color = XMFLOAT4(1.0f, 1.0f, 0.2f, 1.0f); // yellow-ish

        d3d_context->UpdateSubresource(s_cb, 0, nullptr, &cb, 0, 0);
        d3d_context->VSSetConstantBuffers(0, 1, &s_cb);
        d3d_context->PSSetConstantBuffers(0, 1, &s_cb);
        d3d_context->VSSetShader(s_vs, nullptr, 0);
        d3d_context->PSSetShader(s_ps, nullptr, 0);

        UINT stride = sizeof(float) * 6, offset = 0;
        d3d_context->IASetVertexBuffers(0, 1, &s_vbRay, &stride, &offset);
        d3d_context->IASetIndexBuffer(s_ibRay, DXGI_FORMAT_R16_UINT, 0);
        d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3d_context->IASetInputLayout(s_il);
        d3d_context->DrawIndexed(36, 0, 0);
    }

    // ---- Draw a 1 cm red cube at the intersection point (if any) ----
    if (hit) {
        XMVECTOR h = XMLoadFloat3(&hitW);
        float s = 0.01f; // 1 cm
        XMMATRIX worldHit =
            XMMatrixScaling(s, s, s) *
            XMMatrixTranslationFromVector(h);

        ControllerCB cb{};
        XMStoreFloat4x4(&cb.world, XMMatrixTranspose(worldHit));
        XMStoreFloat4x4(&cb.viewproj, XMMatrixTranspose(viewM * proj));
        cb.color = XMFLOAT4(1.0f, 0.1f, 0.1f, 1.0f); // solid red

        d3d_context->UpdateSubresource(s_cb, 0, nullptr, &cb, 0, 0);
        d3d_context->VSSetConstantBuffers(0, 1, &s_cb);
        d3d_context->PSSetConstantBuffers(0, 1, &s_cb);
        d3d_context->VSSetShader(s_vs, nullptr, 0);
        d3d_context->PSSetShader(s_psFlat ? s_psFlat : s_ps, nullptr, 0);

        UINT stride = sizeof(float) * 6, offset = 0;
        d3d_context->IASetVertexBuffers(0, 1, &s_vbCube, &stride, &offset);
        d3d_context->IASetIndexBuffer(s_ibCube, DXGI_FORMAT_R16_UINT, 0);
        d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3d_context->IASetInputLayout(s_il);
        d3d_context->DrawIndexed(36, 0, 0);

        DesktopPlane_WarpCursor(uv);
    }
}

void Controllers_Draw(const XrCompositionLayerProjectionView& view) {
    // Left: blue, Right: red (pick any colors you like)
    DrawOne(view, 0, XMFLOAT4(0.2f, 0.6f, 1.0f, 1.0f));
    DrawOne(view, 1, XMFLOAT4(1.0f, 0.3f, 0.3f, 1.0f));
}

void Controllers_Shutdown() {
    if (s_ibRay) { s_ibRay->Release();  s_ibRay = nullptr; }
    if (s_vbRay) { s_vbRay->Release();  s_vbRay = nullptr; }
    if (s_ibCube) { s_ibCube->Release(); s_ibCube = nullptr; }
    if (s_vbCube) { s_vbCube->Release(); s_vbCube = nullptr; }
    if (s_cb) { s_cb->Release();     s_cb = nullptr; }
    if (s_il) { s_il->Release();     s_il = nullptr; }
    if (s_ps) { s_ps->Release();     s_ps = nullptr; }
    if (s_vs) { s_vs->Release();     s_vs = nullptr; }
    if (s_psFlat) { s_psFlat->Release(); s_psFlat = nullptr; }
}


/*
static void MouseClickAt(POINT pt, bool right) {
    // Move cursor to the point, then click
    SetCursorPos(pt.x, pt.y);

    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[1].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    inputs[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}
*/

// Give focus to the window under the point (best-effort)
static void FocusWindowAt(POINT pt) {
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) return;

    // Climb to the top-level window (some games render in child windows)
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;

    // Allow us to set foreground and do the switch
    DWORD targetTid = GetWindowThreadProcessId(top, nullptr);
    DWORD thisTid = GetCurrentThreadId();
    AttachThreadInput(thisTid, targetTid, TRUE);

    // If minimized, restore
    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);

    AttachThreadInput(thisTid, targetTid, FALSE);
}

// Convert screen px to absolute coords and send move+click via SendInput
static void SendAbsoluteClick(POINT pt, bool right) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Normalize to [0..65535] across the *virtual* desktop
    const double nx = (double)(pt.x - vx) * 65535.0 / (double)(vw - 1);
    const double ny = (double)(pt.y - vy) * 65535.0 / (double)(vh - 1);

    INPUT in[3] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[0].mi.dx = (LONG)nx;
    in[0].mi.dy = (LONG)ny;

    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;

    in[2].type = INPUT_MOUSE;
    in[2].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;

    SendInput(3, in, sizeof(INPUT));
}

static void SendKeyTap(WORD vk) {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;

    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, in, sizeof(INPUT));
}

static void SendWheelAt(POINT pt, int wheelDelta) {
    // Focus the target window (important for games)
    FocusWindowAt(pt);

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = wheelDelta; // +120 = scroll up, -120 = scroll down
    SendInput(1, &in, sizeof(INPUT));
}

// Focus the window under the point, then send the click (it is hard to get games to accept mouse clicks)
static void MouseClickAt(POINT pt, bool right) {
    FocusWindowAt(pt);          // try to give focus to the game/window under the cursor
    SendAbsoluteClick(pt, right);
}

static void SendKeyTap_Scan(WORD vk) {
    // Convert VK -> scan code for the current layout
    HKL layout = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, layout);

    // Some keys require the extended flag (E0). If the scan code has 0xE000 prefix, set EXTENDEDKEY.
    DWORD extFlag = 0;
    if ((sc & 0x0100) != 0) extFlag = KEYEVENTF_EXTENDEDKEY; // crude check; many use a lookup table

    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = (WORD)sc;
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE | extFlag;

    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | extFlag;

    SendInput(2, in, sizeof(INPUT));
}

static void SendCharPressHold(wchar_t ch, DWORD hold_ms = 100) {
    HKL layout = GetKeyboardLayout(0);

    SHORT vkAndMods = VkKeyScanExW(ch, layout);
    if (vkAndMods == -1) return; // char not representable

    BYTE mods = HIBYTE(vkAndMods);
    WORD vk = LOBYTE(vkAndMods);

    auto scOf = [&](WORD vkKey)->UINT {
        return MapVirtualKeyEx(vkKey, MAPVK_VK_TO_VSC_EX, layout);
        };

    INPUT seq[16]{};
    int n = 0;

    // press modifiers
    if (mods & 1) { seq[n].type = INPUT_KEYBOARD; seq[n].ki.wScan = (WORD)scOf(VK_SHIFT); seq[n].ki.dwFlags = KEYEVENTF_SCANCODE; n++; }
    if (mods & 2) { seq[n].type = INPUT_KEYBOARD; seq[n].ki.wScan = (WORD)scOf(VK_CONTROL); seq[n].ki.dwFlags = KEYEVENTF_SCANCODE; n++; }
    if (mods & 4) { seq[n].type = INPUT_KEYBOARD; seq[n].ki.wScan = (WORD)scOf(VK_MENU); seq[n].ki.dwFlags = KEYEVENTF_SCANCODE; n++; }

    // main key down (by SCANCODE)
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, layout);
    seq[n].type = INPUT_KEYBOARD; seq[n].ki.wScan = (WORD)sc; seq[n].ki.dwFlags = KEYEVENTF_SCANCODE; n++;
    SendInput(n, seq, sizeof(INPUT));

    // hold a bit so picky games register it
    if (hold_ms) Sleep(hold_ms);

    // main key up
    INPUT upMain{}; upMain.type = INPUT_KEYBOARD; upMain.ki.wScan = (WORD)sc; upMain.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &upMain, sizeof(INPUT));

    // release modifiers (reverse order)
    INPUT ups[3]{}; int m = 0;
    if (mods & 4) { ups[m].type = INPUT_KEYBOARD; ups[m].ki.wScan = (WORD)scOf(VK_MENU);    ups[m].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP; m++; }
    if (mods & 2) { ups[m].type = INPUT_KEYBOARD; ups[m].ki.wScan = (WORD)scOf(VK_CONTROL); ups[m].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP; m++; }
    if (mods & 1) { ups[m].type = INPUT_KEYBOARD; ups[m].ki.wScan = (WORD)scOf(VK_SHIFT);   ups[m].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP; m++; }
    if (m) SendInput(m, ups, sizeof(INPUT));
}

// --- Non-blocking scan-code key tap scheduler -------------------------------
#include <vector>

static bool NeedsExtended(WORD vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:   // PageUp/PageDown
    case VK_RIGHT:  case VK_LEFT:   case VK_UP:   case VK_DOWN:
    case VK_NUMLOCK: case VK_DIVIDE: case VK_RMENU: case VK_RCONTROL:
        // NOTE: main Enter is not extended; numpad Enter would be, but shares VK_RETURN.
        // If you specifically want numpad Enter-as-extended, set the flag yourself.
        return true;
    default: return false;
    }
}

static void SendKeyDown_Scan(WORD vk) {
    HKL kl = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, kl);
    if (sc == 0) return;
    DWORD flags = KEYEVENTF_SCANCODE | (NeedsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void SendKeyUp_Scan(WORD vk) {
    HKL kl = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, kl);
    if (sc == 0) return;
    DWORD flags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (NeedsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

struct KeyTapTask {
    WORD        vk;
    ULONGLONG   upTimeMs;
    bool        downSent;
};

static std::vector<KeyTapTask> s_keyTapQueue;

// Schedule a tap; sends DOWN now, UP after hold_ms (via PumpScheduledKeys per frame)
static void ScheduleKeyTap_Scan(WORD vk, DWORD hold_ms = 100) {
    // Send DOWN immediately (non-blocking)
    SendKeyDown_Scan(vk);

    KeyTapTask t{};
    t.vk = vk;
    t.upTimeMs = GetTickCount64() + hold_ms;
    t.downSent = true;
    s_keyTapQueue.push_back(t);
}

// Call this once per frame (e.g., in your input/update loop)
static void PumpScheduledKeys() {
    if (s_keyTapQueue.empty()) return;
    ULONGLONG now = GetTickCount64();

    // compact in-place
    size_t w = 0;
    for (size_t i = 0; i < s_keyTapQueue.size(); ++i) {
        auto& task = s_keyTapQueue[i];
        if (task.downSent && now >= task.upTimeMs) {
            SendKeyUp_Scan(task.vk);
            // drop it
        }
        else {
            s_keyTapQueue[w++] = task;
        }
    }
    s_keyTapQueue.resize(w);
}

// Return screen position of controller raycast hit, or std::nullopt if no hit
std::optional<POINT> get_cursor_pos(bool right_hand = true) {
    using namespace DirectX;

    int hand = right_hand ? 1 : 0;

    // Load pose orientation and position
    XMVECTOR q = XMLoadFloat4((XMFLOAT4*)&xr_input.handPose[hand].orientation);
    XMVECTOR p = XMLoadFloat3((XMFLOAT3*)&xr_input.handPose[hand].position);

    // Apply tilt
    const float tiltDeg = -35.0f;
    XMVECTOR tq = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(tiltDeg));
    XMVECTOR qTilt = XMQuaternionMultiply(tq, q);

    // Direction of the ray in world space
    XMVECTOR dirW = XMVector3Normalize(XMVector3Rotate(XMVectorSet(0, 0, -1, 0), qTilt));

    DirectX::XMFLOAT3 hitW;
    DirectX::XMFLOAT2 uv;

    // Raycast toward the desktop plane
    if (DesktopPlane_Raycast(p, dirW, &hitW, &uv)) {
        POINT pt{};
        if (DesktopPlane_UVToScreen(uv, &pt)) {
            return pt;
        }
    }

    return std::nullopt;
}

