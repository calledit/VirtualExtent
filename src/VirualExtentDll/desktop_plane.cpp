#include "pch.h"
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <cmath>
#include <Windows.h>
#undef min
#undef max
#include <algorithm>

#include "desktop_plane.h"
#include "desktop_capture.h"
#include "d3d.h" // d3d_device, d3d_context, d3d_xr_projection, d3d_compile_shader


using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------- Module state ----------------

struct PlaneVertex { XMFLOAT3 pos; XMFLOAT2 uv; };

static ID3D11VertexShader* s_vs = nullptr;
static ID3D11PixelShader* s_ps = nullptr;
static ID3D11InputLayout* s_il = nullptr;
static ID3D11Buffer* s_vb = nullptr;
static ID3D11Buffer* s_ib = nullptr;
static ID3D11SamplerState* s_samp = nullptr;
static ID3D11RasterizerState* s_rs = nullptr;
static ID3D11Buffer* s_cb = nullptr; // our own constant buffer

static bool     s_placed = false;
static XMFLOAT4X4 s_worldStored{};
static float    s_widthMeters = 3.0f;
static float    s_distance = 1.5f;

static DesktopCapture s_capture;
static ID3D11ShaderResourceView* s_srv = nullptr;
static uint32_t s_w = 0, s_h = 0;
static int s_outputIndex = 0;



// HLSL for the textured plane (mirror U to fix left-right inversion)
static constexpr char kPlaneShaderHLSL[] = R"_(
cbuffer TransformBuffer : register(b0) {
    float4x4 world;
    float4x4 viewproj;
};
struct vsIn { float3 pos:POSITION; float2 uv:TEXCOORD0; };
struct psIn { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
psIn vs(vsIn i) {
    psIn o;
    o.pos = mul(float4(i.pos,1), world);
    o.pos = mul(o.pos, viewproj);
    o.uv  = i.uv;
    return o;
}
Texture2D tex0 : register(t0);
SamplerState s0 : register(s0);
float4 ps(psIn i) : SV_TARGET { return tex0.Sample(s0, i.uv); }
)_";

static RECT s_outputRect = { 0,0,0,0 }; // desktop coords of the captured output
extern uint32_t s_w, s_h;             // your existing capture width/height

// ------------------------------------------------

static bool InitCaptureOnSameAdapter(int outputIndex) {
	// Grab the adapter used by our D3D11 device
	IDXGIDevice* dxgiDev = nullptr;
	IDXGIAdapter* dxgiAdapter = nullptr;
	if (FAILED(d3d_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)))
		return false;
	dxgiDev->GetAdapter(&dxgiAdapter);

	ComPtr<IDXGIAdapter1> a1;
	dxgiAdapter->QueryInterface(__uuidof(IDXGIAdapter1), (void**)a1.GetAddressOf());
	dxgiAdapter->Release();
	dxgiDev->Release();

	return s_capture.Init(d3d_device, a1.Get(), outputIndex);
}

bool DesktopPlane_Init(float widthMeters, float distanceMeters, int outputIndex) {
	s_widthMeters = widthMeters;
	s_distance = distanceMeters;
	s_outputIndex = outputIndex;
	s_placed = false;
	if (s_srv) { s_srv->Release(); s_srv = nullptr; }
	s_w = s_h = 0;

	// Shaders
	ID3DBlob* vsb = d3d_compile_shader(kPlaneShaderHLSL, "vs", "vs_5_0");
	ID3DBlob* psb = d3d_compile_shader(kPlaneShaderHLSL, "ps", "ps_5_0");
	if (!vsb || !psb) return false;

	d3d_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_vs);
	d3d_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ps);

	D3D11_INPUT_ELEMENT_DESC il[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	d3d_device->CreateInputLayout(il, (UINT)_countof(il), vsb->GetBufferPointer(), vsb->GetBufferSize(), &s_il);
	vsb->Release(); psb->Release();

	// Plane geometry (XY unit quad)
	PlaneVertex verts[4] = {
		{{-0.5f,-0.5f,0.0f}, {0.0f,1.0f}},
		{{ 0.5f,-0.5f,0.0f}, {1.0f,1.0f}},
		{{ 0.5f, 0.5f,0.0f}, {1.0f,0.0f}},
		{{-0.5f, 0.5f,0.0f}, {0.0f,0.0f}},
	};
	uint16_t idx[6] = { 0,1,2, 0,2,3 };

	D3D11_SUBRESOURCE_DATA vbData{ verts };
	D3D11_BUFFER_DESC      vbDesc = CD3D11_BUFFER_DESC(sizeof(verts), D3D11_BIND_VERTEX_BUFFER);
	d3d_device->CreateBuffer(&vbDesc, &vbData, &s_vb);

	D3D11_SUBRESOURCE_DATA ibData{ idx };
	D3D11_BUFFER_DESC      ibDesc = CD3D11_BUFFER_DESC(sizeof(idx), D3D11_BIND_INDEX_BUFFER);
	d3d_device->CreateBuffer(&ibDesc, &ibData, &s_ib);

	// Constant buffer (our own)
	D3D11_BUFFER_DESC cbDesc = CD3D11_BUFFER_DESC(sizeof(XMFLOAT4X4) * 2, D3D11_BIND_CONSTANT_BUFFER);
	d3d_device->CreateBuffer(&cbDesc, nullptr, &s_cb);

	// Sampler
	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
	d3d_device->CreateSamplerState(&sd, &s_samp);

	// Rasterizer: disable culling
	D3D11_RASTERIZER_DESC rd{};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.FrontCounterClockwise = FALSE;
	rd.DepthClipEnable = TRUE;
	d3d_device->CreateRasterizerState(&rd, &s_rs);

	// Desktop duplication on same adapter/output
	if (!InitCaptureOnSameAdapter(s_outputIndex))
		return false;

	// --- NEW: record desktop coordinates of the chosen output for cursor warping ---
	{
		Microsoft::WRL::ComPtr<IDXGIDevice>  dxgiDev;
		Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
		if (SUCCEEDED(d3d_device->QueryInterface(IID_PPV_ARGS(&dxgiDev))) &&
			SUCCEEDED(dxgiDev->GetAdapter(&dxgiAdapter)))
		{
			Microsoft::WRL::ComPtr<IDXGIAdapter1> a1;
			dxgiAdapter.As(&a1);

			Microsoft::WRL::ComPtr<IDXGIOutput> out;
			if (a1 && SUCCEEDED(a1->EnumOutputs(s_outputIndex, &out))) {
				DXGI_OUTPUT_DESC od{};
				if (SUCCEEDED(out->GetDesc(&od))) {
					s_outputRect = od.DesktopCoordinates; // virtual-desktop coords for this monitor
				}
			}
		}
	}
	// ------------------------------------------------------------------------------

	return true;
}


bool DesktopPlane_UVToScreen(const DirectX::XMFLOAT2& uv, POINT* outScreenPt) {
	if (s_w == 0 || s_h == 0) return false;

	// Clamp UV to [0..1]
	float u = uv.x; if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
	float v = uv.y; if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;

	// Flip U to compensate for the shader's horizontal mirror (o.uv = float2(1 - i.uv.x, i.uv.y))
	u = 1.0f - u;

	// UV → pixels on the captured output
	int px = int(u * float(s_w) + 0.5f);
	int py = int(v * float(s_h) + 0.5f);

	// Pixels → virtual desktop coords on that monitor
	int sx = s_outputRect.left + px;
	int sy = s_outputRect.top + py;

	if (outScreenPt) { outScreenPt->x = sx; outScreenPt->y = sy; }
	return true;
}


bool DesktopPlane_WarpCursor(const DirectX::XMFLOAT2& uv) {
	POINT pt{};
	if (!DesktopPlane_UVToScreen(uv, &pt)) return false;
	return SetCursorPos(pt.x, pt.y) ? true : false;
}

void DesktopPlane_Draw(const XrCompositionLayerProjectionView& view) {
	// Acquire latest desktop frame (non-blocking)
	ID3D11ShaderResourceView* newSrv = nullptr;
	uint32_t newW = 0, newH = 0;
	if (s_capture.Acquire(d3d_context, &newSrv, &newW, &newH, 0)) {
		if (s_srv) s_srv->Release();
		s_srv = newSrv;
		s_w = newW; s_h = newH;
	}
	if (!s_srv) return;

	// Per-view matrices
	XMVECTOR camOri = XMLoadFloat4((XMFLOAT4*)&view.pose.orientation);
	XMVECTOR camPos = XMLoadFloat3((XMFLOAT3*)&view.pose.position);
	XMMATRIX proj = d3d_xr_projection(view.fov, 0.05f, 100.0f);
	XMMATRIX viewM = XMMatrixInverse(nullptr, XMMatrixAffineTransformation(
		DirectX::g_XMOne, DirectX::g_XMZero, camOri, camPos));

	// Place once from initial gaze
	if (!s_placed) {
		// Size from desktop aspect
		float aspect = s_h ? (float)s_w / (float)s_h : (16.0f / 9.0f);
		float width = s_widthMeters;
		float height = width / aspect;

		// Initial forward from HMD, but project to XZ (kill pitch/roll)
		XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), camOri);
		XMVECTOR fwdXZ = XMVector3Normalize(XMVectorSet(XMVectorGetX(fwd), 0.0f, XMVectorGetZ(fwd), 0.0f));

		// Place the plane in front of the origin along yaw-only forward
		XMVECTOR planePos = XMVectorScale(fwdXZ, s_distance);

		// Compute yaw (so +Z of the quad faces the user at origin)
		float fx = XMVectorGetX(fwdXZ);
		float fz = XMVectorGetZ(fwdXZ);
		float yaw = atan2f(fx, fz) + XM_PI; // rotate +Z to -fwdXZ

		// World = Scale * YawOnly * Translate; no roll/pitch, so it won't lean
		XMMATRIX worldOnce =
			XMMatrixScaling(width, height, 1.0f) *
			XMMatrixRotationY(yaw) *
			XMMatrixTranslationFromVector(planePos);

		XMStoreFloat4x4(&s_worldStored, worldOnce);
		s_placed = true;
	}


	XMMATRIX world = XMLoadFloat4x4(&s_worldStored);

	// Update our own constant buffer
	struct { XMFLOAT4X4 world; XMFLOAT4X4 viewproj; } cbData;
	XMStoreFloat4x4(&cbData.world, XMMatrixTranspose(world));
	XMStoreFloat4x4(&cbData.viewproj, XMMatrixTranspose(viewM * proj));
	d3d_context->UpdateSubresource(s_cb, 0, nullptr, &cbData, 0, 0);

	// Set rasterizer (cull none) temporarily
	ID3D11RasterizerState* prevRS = nullptr;
	d3d_context->RSGetState(&prevRS);
	d3d_context->RSSetState(s_rs);

	// Bind pipeline
	UINT stride = sizeof(PlaneVertex), offset = 0;
	d3d_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
	d3d_context->IASetIndexBuffer(s_ib, DXGI_FORMAT_R16_UINT, 0);
	d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d3d_context->IASetInputLayout(s_il);
	d3d_context->VSSetShader(s_vs, nullptr, 0);
	d3d_context->PSSetShader(s_ps, nullptr, 0);
	d3d_context->VSSetConstantBuffers(0, 1, &s_cb);
	d3d_context->PSSetSamplers(0, 1, &s_samp);
	d3d_context->PSSetShaderResources(0, 1, &s_srv);

	d3d_context->DrawIndexed(6, 0, 0);

	// Unbind & restore
	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	d3d_context->PSSetShaderResources(0, 1, nullSRV);
	d3d_context->RSSetState(prevRS);
	if (prevRS) prevRS->Release();
}

void DesktopPlane_Shutdown() {
	if (s_srv) { s_srv->Release(); s_srv = nullptr; }
	s_capture.Shutdown();

	if (s_vs) { s_vs->Release();   s_vs = nullptr; }
	if (s_ps) { s_ps->Release();   s_ps = nullptr; }
	if (s_il) { s_il->Release();   s_il = nullptr; }
	if (s_vb) { s_vb->Release();   s_vb = nullptr; }
	if (s_ib) { s_ib->Release();   s_ib = nullptr; }
	if (s_samp) { s_samp->Release(); s_samp = nullptr; }
	if (s_rs) { s_rs->Release();   s_rs = nullptr; }
	if (s_cb) { s_cb->Release();   s_cb = nullptr; }

	s_placed = false;
	s_w = s_h = 0;
}




bool DesktopPlane_Raycast(const DirectX::XMVECTOR& rayOrigin,
	const DirectX::XMVECTOR& rayDir,
	DirectX::XMFLOAT3* outWorldHit,
	DirectX::XMFLOAT2* outUV)
{
	using namespace DirectX;
	if (!s_placed) return false;

	// Inverse world to get into plane-local space (quad lies on local z=0, x/y in [-0.5,0.5])
	XMMATRIX W = XMLoadFloat4x4(&s_worldStored);
	XMMATRIX Inv = XMMatrixInverse(nullptr, W);

	// Transform ray into local space
	XMVECTOR oL = XMVector3TransformCoord(rayOrigin, Inv);
	XMVECTOR dL = XMVector3TransformNormal(rayDir, Inv);
	float dz = XMVectorGetZ(dL);
	if (fabsf(dz) < 1e-6f) return false; // parallel to plane

	float t = -XMVectorGetZ(oL) / dz;    // plane is z=0
	if (t < 0.0f) return false;          // behind origin

	XMVECTOR hitL = XMVectorAdd(oL, XMVectorScale(dL, t));
	float x = XMVectorGetX(hitL);
	float y = XMVectorGetY(hitL);

	// inside quad?
	if (x < -0.5f || x > 0.5f || y < -0.5f || y > 0.5f)
		return false;

	// Back to world
	XMVECTOR hitW = XMVector3TransformCoord(hitL, W);
	if (outWorldHit) XMStoreFloat3(outWorldHit, hitW);

	// UV mapping that matches geometry + shader (shader mirrors U: u' = 1-u)
	// Geometry mapping: u = x+0.5, v = 1-(y+0.5)
	float u_geom = x + 0.5f;
	float v_geom = 1.0f - (y + 0.5f);
	float u_sample = 1.0f - u_geom; // mirror horizontally, like the shader
	if (outUV) *outUV = XMFLOAT2(u_sample, v_geom);

	return true;
}