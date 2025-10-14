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

void Controllers_UpdatePredicted() {
    // No persistent state; we read xr_input.handPose each draw.
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

void Controllers_HandleClicks() {
    // For each hand, if a button edge fired, raycast and click if hitting the desktop
    for (int hand = 0; hand < 2; ++hand) {
        bool doLeft = xr_input.handSelect[hand] ? true : false;
        bool doRight = xr_input.handSecondary[hand] ? true : false;
        if (!doLeft && !doRight) continue;

        // Build a slightly-down-tilted ray from the grip pose (same as you draw)
        const float tiltDeg = -35.0f;
        using namespace DirectX;
        XMVECTOR q = XMLoadFloat4((XMFLOAT4*)&xr_input.handPose[hand].orientation);
        XMVECTOR p = XMLoadFloat3((XMFLOAT3*)&xr_input.handPose[hand].position);
        XMVECTOR tq = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), XMConvertToRadians(tiltDeg));
        XMVECTOR qTilt = XMQuaternionMultiply(tq, q);
        XMVECTOR dirW = XMVector3Normalize(XMVector3Rotate(XMVectorSet(0, 0, -1, 0), qTilt));

        DirectX::XMFLOAT3 hitW;
        DirectX::XMFLOAT2 uv;
        if (DesktopPlane_Raycast(p, dirW, &hitW, &uv)) {
            POINT pt{};
            if (DesktopPlane_UVToScreen(uv, &pt)) {
                if (doLeft)  MouseClickAt(pt, /*right=*/false);
                if (doRight) MouseClickAt(pt, /*right=*/true);
            }
        }
    }
}
