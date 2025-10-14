#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <vector>

#include "scene_cubes.h"
#include "d3d.h"     // d3d_device, d3d_context, d3d_xr_projection, d3d_compile_shader
#include "openxr.h"  // xr_input, xr_pose_identity

using namespace DirectX;
using std::vector;

// ---------------- Module state ----------------

struct TransformCB { XMFLOAT4X4 world; XMFLOAT4X4 viewproj; };

static ID3D11VertexShader* s_vs = nullptr;
static ID3D11PixelShader* s_ps = nullptr;
static ID3D11InputLayout* s_il = nullptr;
static ID3D11Buffer* s_cb = nullptr;
static ID3D11Buffer* s_vb = nullptr;
static ID3D11Buffer* s_ib = nullptr;

static vector<XrPosef>     s_cubes;

// HLSL (same as your original app shader)
static constexpr char kCubeShaderHLSL[] = R"_(
cbuffer TransformBuffer : register(b0) {
    float4x4 world;
    float4x4 viewproj;
};
struct vsIn {
    float4 pos  : SV_POSITION;
    float3 norm : NORMAL;
};
struct psIn {
    float4 pos   : SV_POSITION;
    float3 color : COLOR0;
};

psIn vs(vsIn input) {
    psIn output;
    output.pos = mul(float4(input.pos.xyz, 1), world);
    output.pos = mul(output.pos, viewproj);

    float3 normal = normalize(mul(float4(input.norm, 0), world).xyz);

    output.color = saturate(dot(normal, float3(0,1,0))).xxx;
    return output;
}
float4 ps(psIn input) : SV_TARGET {
    return float4(input.color, 1);
})_";

// Same cube geometry as before: [pos.xyz, norm.xyz] interleaved
static float s_verts[] = {
	-1,-1,-1, -1,-1,-1,
	 1,-1,-1,  1,-1,-1,
	 1, 1,-1,  1, 1,-1,
	-1, 1,-1, -1, 1,-1,
	-1,-1, 1, -1,-1, 1,
	 1,-1, 1,  1,-1, 1,
	 1, 1, 1,  1, 1, 1,
	-1, 1, 1, -1, 1, 1, };

static uint16_t s_inds[] = {
	1,2,0, 2,3,0, 4,6,5, 7,6,4,
	6,2,1, 5,6,1, 3,7,4, 0,3,4,
	4,5,1, 0,4,1, 2,7,3, 2,6,7, };

// ------------------------------------------------

bool Cubes_Init() {
	// Shaders
	ID3DBlob* vsb = d3d_compile_shader(kCubeShaderHLSL, "vs", "vs_5_0");
	ID3DBlob* psb = d3d_compile_shader(kCubeShaderHLSL, "ps", "ps_5_0");
	if (!vsb || !psb) return false;

	d3d_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_vs);
	d3d_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ps);

	D3D11_INPUT_ELEMENT_DESC il[] = {
		{"SV_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"NORMAL",      0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	d3d_device->CreateInputLayout(il, (UINT)_countof(il), vsb->GetBufferPointer(), vsb->GetBufferSize(), &s_il);
	vsb->Release(); psb->Release();

	// Buffers
	D3D11_SUBRESOURCE_DATA vbData = { s_verts };
	D3D11_SUBRESOURCE_DATA ibData = { s_inds };
	D3D11_BUFFER_DESC vbDesc = CD3D11_BUFFER_DESC(sizeof(s_verts), D3D11_BIND_VERTEX_BUFFER);
	D3D11_BUFFER_DESC ibDesc = CD3D11_BUFFER_DESC(sizeof(s_inds), D3D11_BIND_INDEX_BUFFER);
	D3D11_BUFFER_DESC cbDesc = CD3D11_BUFFER_DESC(sizeof(TransformCB), D3D11_BIND_CONSTANT_BUFFER);

	d3d_device->CreateBuffer(&vbDesc, &vbData, &s_vb);
	d3d_device->CreateBuffer(&ibDesc, &ibData, &s_ib);
	d3d_device->CreateBuffer(&cbDesc, nullptr, &s_cb);

	return true;
}

void Cubes_Update() {
	// If the user presses the select action, add a cube at that location
	for (uint32_t i = 0; i < 2; i++) {
		if (xr_input.handSelect[i])
			s_cubes.push_back(xr_input.handPose[i]);
	}
}

void Cubes_UpdatePredicted() {
	// Keep showing cubes on hands (first two) at predicted pose
	if (s_cubes.size() < 2)
		s_cubes.resize(2, xr_pose_identity);
	for (uint32_t i = 0; i < 2; i++) {
		s_cubes[i] = xr_input.renderHand[i] ? xr_input.handPose[i] : xr_pose_identity;
	}
}

void Cubes_Draw(const XrCompositionLayerProjectionView& view) {
	// Build per-eye matrices
	XMMATRIX proj = d3d_xr_projection(view.fov, 0.05f, 100.0f);
	XMMATRIX viewM = XMMatrixInverse(nullptr, XMMatrixAffineTransformation(
		DirectX::g_XMOne, DirectX::g_XMZero,
		XMLoadFloat4((XMFLOAT4*)&view.pose.orientation),
		XMLoadFloat3((XMFLOAT3*)&view.pose.position)));

	// Pipeline
	d3d_context->VSSetConstantBuffers(0, 1, &s_cb);
	d3d_context->VSSetShader(s_vs, nullptr, 0);
	d3d_context->PSSetShader(s_ps, nullptr, 0);

	UINT stride = sizeof(float) * 6, offset = 0;
	d3d_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
	d3d_context->IASetIndexBuffer(s_ib, DXGI_FORMAT_R16_UINT, 0);
	d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d3d_context->IASetInputLayout(s_il);

	// viewproj once
	TransformCB cb{};
	XMStoreFloat4x4(&cb.viewproj, XMMatrixTranspose(viewM * proj));

	// Draw all cubes
	for (size_t i = 0; i < s_cubes.size(); i++) {
		XMMATRIX model = XMMatrixAffineTransformation(
			DirectX::g_XMOne * 0.05f, DirectX::g_XMZero,
			XMLoadFloat4((XMFLOAT4*)&s_cubes[i].orientation),
			XMLoadFloat3((XMFLOAT3*)&s_cubes[i].position));

		XMStoreFloat4x4(&cb.world, XMMatrixTranspose(model));
		d3d_context->UpdateSubresource(s_cb, 0, nullptr, &cb, 0, 0);
		d3d_context->DrawIndexed((UINT)_countof(s_inds), 0, 0);
	}
}

void Cubes_Shutdown() {
	if (s_cb) { s_cb->Release(); s_cb = nullptr; }
	if (s_ib) { s_ib->Release(); s_ib = nullptr; }
	if (s_vb) { s_vb->Release(); s_vb = nullptr; }
	if (s_il) { s_il->Release(); s_il = nullptr; }
	if (s_ps) { s_ps->Release(); s_ps = nullptr; }
	if (s_vs) { s_vs->Release(); s_vs = nullptr; }
	s_cubes.clear();
}
