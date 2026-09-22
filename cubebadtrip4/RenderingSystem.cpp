#include "RenderingSystem.h"

#include "d3dUtil.h"
#include "d3dx12.h"

#include <cstddef>
#include <cstdint>

namespace
{
    constexpr UINT LightRootConstantCount = 16;

    DeferredLight MakeLight(
        float px, float py, float pz, float range,
        float dx, float dy, float dz, float intensity,
        float r, float g, float b, DeferredLightType type,
        float innerConeCos = 0.0f,
        float outerConeCos = 0.0f)
    {
        DeferredLight light = {};
        light.PositionRange = DirectX::XMFLOAT4(px, py, pz, range);
        light.DirectionIntensity = DirectX::XMFLOAT4(dx, dy, dz, intensity);
        light.ColorType = DirectX::XMFLOAT4(
            r,
            g,
            b,
            static_cast<float>(type));
        light.SpotParams = DirectX::XMFLOAT4(
            innerConeCos,
            outerConeCos,
            0.0f,
            0.0f);
        return light;
    }
}

static_assert(sizeof(DeferredLight) == sizeof(float) * LightRootConstantCount,
    "DeferredLight must match the 16 root constants used by the shader.");

bool RenderingSystem::Initialize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT backBufferFormat)
{
    if (device == nullptr)
    {
        return false;
    }

    mDevice = device;

    if (!mGbuffer.Initialize(device, width, height))
    {
        return false;
    }

    BuildRootSignature();
    BuildPipelineStates(backBufferFormat);
    BuildDefaultLights();

    mInitialized = true;
    return true;
}

void RenderingSystem::Resize(UINT width, UINT height)
{
    if (mInitialized)
    {
        mGbuffer.Resize(width, height);
    }
}

void RenderingSystem::BeginGeometryPass(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView)
{
    if (mInitialized)
    {
        mGbuffer.BeginGeometryPass(commandList, depthStencilView);
    }
}

void RenderingSystem::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE gbufferRange;
    gbufferRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        Gbuffer::TargetCount,
        0);

    CD3DX12_ROOT_PARAMETER rootParameters[2];
    rootParameters[0].InitAsDescriptorTable(
        1,
        &gbufferRange,
        D3D12_SHADER_VISIBILITY_PIXEL);
    rootParameters[1].InitAsConstants(
        LightRootConstantCount,
        0,
        0,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(
        _countof(rootParameters),
        rootParameters,
        0,
        nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSignature.GetAddressOf(),
        errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        OutputDebugStringA(static_cast<const char*>(
            errorBlob->GetBufferPointer()));
    }
    ThrowIfFailed(serializeResult);

    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serializedRootSignature->GetBufferPointer(),
        serializedRootSignature->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void RenderingSystem::BuildPipelineStates(DXGI_FORMAT backBufferFormat)
{
    mVertexShader = d3dUtil::CompileShader(
        L"Shaders\\DeferredLighting.hlsl",
        nullptr,
        "VS",
        "vs_5_0");

    mPixelShader = d3dUtil::CompileShader(
        L"Shaders\\DeferredLighting.hlsl",
        nullptr,
        "PS",
        "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS =
    {
        reinterpret_cast<BYTE*>(
            mVertexShader->GetBufferPointer()),
        mVertexShader->GetBufferSize()
    };

    psoDesc.PS =
    {
        reinterpret_cast<BYTE*>(
            mPixelShader->GetBufferPointer()),
        mPixelShader->GetBufferSize()
    };
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = backBufferFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &psoDesc,
        IID_PPV_ARGS(mAmbientPso.GetAddressOf())));

    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &psoDesc,
        IID_PPV_ARGS(mAdditivePso.GetAddressOf())));
}

void RenderingSystem::BuildDefaultLights()
{
    mLights.clear();

    // Направленный жёлто-оранжевый свет
    mLights.push_back(MakeLight(
        0.0f, 0.0f, 0.0f, 0.0f,
        -0.2f, -1.0f, 0.15f, 0.45f,
        1.0f, 0.50f, 0.05f,
        DeferredLightType::Directional));

    // Точечный красный свет слева
    mLights.push_back(MakeLight(
        -850.0f, 100.0f, -100.0f, 1350.0f,
        0.0f, 0.0f, 0.0f, 2.7f,
        1.0f, 0.03f, 0.02f,
        DeferredLightType::Point));

    // Точечный синий свет справа
    mLights.push_back(MakeLight(
        850.0f, 300.0f, 250.0f, 1350.0f,
        0.0f, 0.0f, 0.0f, 2.7f,
        0.02f, 0.08f, 1.0f,
        DeferredLightType::Point));

    // Точечный зелёный свет спереди
    mLights.push_back(MakeLight(
        0.0f, -250.0f, 650.0f, 1200.0f,
        0.0f, 0.0f, 0.0f, 2.5f,
        0.02f, 1.0f, 0.04f,
        DeferredLightType::Point));

    // Фиолетовый прожектор
    mLights.push_back(MakeLight(
        0.0f, 680.0f, -900.0f, 2200.0f,
        0.0f, -0.45f, 1.0f, 3.2f,
        1.0f, 0.05f, 0.9f,
        DeferredLightType::Spot,
        0.85f,
        0.60f));
}

void RenderingSystem::DrawLight(
    ID3D12GraphicsCommandList* commandList,
    const DeferredLight& light)
{
    commandList->SetGraphicsRoot32BitConstants(
        1,
        LightRootConstantCount,
        &light,
        0);
    commandList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::RenderLighting(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissorRect)
{
    if (!mInitialized || commandList == nullptr || backBuffer == nullptr)
    {
        return;
    }

    mGbuffer.EndGeometryPass(commandList);

    auto backBufferToRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &backBufferToRenderTarget);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(backBufferRtv, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* descriptorHeaps[] = { mGbuffer.GetSrvHeap() };
    commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetGraphicsRootDescriptorTable(
        0,
        mGbuffer.GetSrvTableStart());

    DeferredLight ambient = {};
    ambient.DirectionIntensity.w = 0.04f;
    ambient.ColorType.w = 3.0f;
    commandList->SetPipelineState(mAmbientPso.Get());
    DrawLight(commandList, ambient);

    commandList->SetPipelineState(mAdditivePso.Get());
    for (const DeferredLight& light : mLights)
    {
        DrawLight(commandList, light);
    }

    auto backBufferToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &backBufferToPresent);
}
