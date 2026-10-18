#include "opaque_pass.h"

#include "../core/log.h"
#include "../hooks/d3d_probe.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>

#include <cstring>

namespace avp::opaque {
namespace {

constexpr const char* kLog = "avpvr_stereo.log";

constexpr char kShader[] = R"(
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS() : SV_Target { return float4(0, 0, 0, 1); }
)";

using PFN_D3DCompile = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*,
                                        const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Texture2D* g_target = nullptr; // the texture g_rtv is for (compared only)
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_failed = false;

template <typename T>
void SafeRelease(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

ID3DBlob* Compile(PFN_D3DCompile compile, const char* entry, const char* profile)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(kShader, sizeof(kShader) - 1, "avpvr_opaque", nullptr, nullptr, entry, profile, 0, 0,
                               &code, &errors);
    if (FAILED(hr))
        AVP_LOG(kLog, "opaque pass: %s failed to compile (%08lX): %s", entry, static_cast<unsigned long>(hr),
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    SafeRelease(errors);
    return SUCCEEDED(hr) ? code : nullptr;
}

bool Setup(ID3D11Device* device)
{
    if (g_device == device && g_vs)
        return true;
    if (g_failed)
        return false;
    g_failed = true; // until everything below worked
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!compile)
    {
        AVP_LOG(kLog, "opaque pass: d3dcompiler_47.dll / D3DCompile not found");
        return false;
    }
    ID3DBlob* vs = Compile(compile, "VS", "vs_4_0");
    ID3DBlob* ps = Compile(compile, "PS", "ps_4_0");
    bool ok = vs && ps &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
    SafeRelease(vs);
    SafeRelease(ps);

    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].BlendEnable = FALSE;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device->CreateBlendState(&b, &g_blend)) &&
         SUCCEEDED(device->CreateDepthStencilState(&d, &g_depth)) &&
         SUCCEEDED(device->CreateRasterizerState(&r, &g_raster));
    if (!ok)
    {
        AVP_LOG(kLog, "opaque pass: setup failed");
        SafeRelease(g_vs);
        SafeRelease(g_ps);
        SafeRelease(g_blend);
        SafeRelease(g_depth);
        SafeRelease(g_raster);
        return false;
    }
    g_device = device;
    g_failed = false;
    AVP_LOG(kLog, "opaque pass ready (the flat screen's alpha forced to 1)");
    return true;
}

// The pipeline state the pass touches.
struct Saved
{
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

    void Save(ID3D11DeviceContext* c)
    {
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->RSGetViewports(&viewportCount, viewports);
    }
    void Restore(ID3D11DeviceContext* c)
    {
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        c->VSSetShader(vs, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->RSSetState(raster);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        c->RSSetViewports(viewportCount, viewports);
        SafeRelease(layout);
        SafeRelease(vs);
        SafeRelease(gs);
        SafeRelease(hs);
        SafeRelease(ds);
        SafeRelease(ps);
        SafeRelease(blend);
        SafeRelease(depth);
        SafeRelease(raster);
        for (ID3D11RenderTargetView*& v : rtvs)
            SafeRelease(v);
        SafeRelease(dsv);
    }
};

} // namespace

bool MakeOpaque(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* texture)
{
    if (!device || !ctx || !texture || !Setup(device))
        return false;
    if (texture != g_target || !g_rtv)
    {
        SafeRelease(g_rtv);
        g_target = nullptr;
        if (FAILED(device->CreateRenderTargetView(texture, nullptr, &g_rtv)))
        {
            static bool logged = false;
            if (!logged)
                AVP_LOG(kLog, "opaque pass: CreateRenderTargetView failed");
            logged = true;
            return false;
        }
        g_target = texture;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    Saved saved;
    saved.Save(ctx);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f};
    ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    const float factor[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depth, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    SetModDraws(true);
    ctx->Draw(3, 0);
    SetModDraws(false);
    saved.Restore(ctx);
    return true;
}

} // namespace avp::opaque
