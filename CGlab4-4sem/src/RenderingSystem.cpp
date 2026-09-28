#include "RenderingSystem.h"

#include "d3dUtil.h"
#include "../h/d3dx12.h"

#include <array>

using Microsoft::WRL::ComPtr;

bool RenderingSystem::Initialize(ID3D12Device* device,
                                 unsigned int width,
                                 unsigned int height,
                                 DXGI_FORMAT backBufferFormat,
                                 DXGI_FORMAT depthStencilFormat) {
    mWidth = width;
    mHeight = height;
    mBackBufferFormat = backBufferFormat;
    mDepthStencilFormat = depthStencilFormat;

    mGBuffer = std::make_unique<GBuffer>();
    if (!mGBuffer->Initialize(device, width, height)) {
        return false;
    }

    BuildRootSignatures(device);
    BuildPSOs(device);
    return true;
}

void RenderingSystem::OnResize(ID3D12Device* device, unsigned int width, unsigned int height) {
    mWidth = width;
    mHeight = height;
    if (mGBuffer) {
        mGBuffer->OnResize(device, width, height);
    }
}

void RenderingSystem::BuildRootSignatures(ID3D12Device* device) {
    {
        CD3DX12_DESCRIPTOR_RANGE ranges[3];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

        CD3DX12_ROOT_PARAMETER params[5];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsConstantBufferView(1);
        params[2].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
        params[3].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
        params[4].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_STATIC_SAMPLER_DESC linearWrap(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            5,
            params,
            1,
            &linearWrap,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized,
            &errors),
            "Serialize geometry root signature failed");

        ThrowIfFailed(device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&mGeometryRootSignature)),
            "Create geometry root signature failed");
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstantBufferView(0);
        params[2].InitAsConstantBufferView(1);

        CD3DX12_STATIC_SAMPLER_DESC linearClamp(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            3,
            params,
            1,
            &linearClamp,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized,
            &errors),
            "Serialize lighting root signature failed");

        ThrowIfFailed(device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&mLightingRootSignature)),
            "Create lighting root signature failed");
    }
}

void RenderingSystem::BuildPSOs(ID3D12Device* device) {
    auto vsGeom = d3dUtil::CompileShader(L"src/main_shader.hlsl", nullptr, "VS_Geometry", "vs_5_0");
    auto vsCp = d3dUtil::CompileShader(L"src/main_shader.hlsl", nullptr, "VS_ControlPoint", "vs_5_0");
    auto hs = d3dUtil::CompileShader(L"src/main_shader.hlsl", nullptr, "HS_Main", "hs_5_0");
    auto ds = d3dUtil::CompileShader(L"src/main_shader.hlsl", nullptr, "DS_Main", "ds_5_0");
    auto psGeom = d3dUtil::CompileShader(L"src/main_shader.hlsl", nullptr, "PS_Geometry", "ps_5_0");

    auto vsLighting = d3dUtil::CompileShader(L"src/lighting.hlsl", nullptr, "VS_Fullscreen", "vs_5_0");
    auto psLighting = d3dUtil::CompileShader(L"src/lighting.hlsl", nullptr, "PS_Lighting", "ps_5_0");

    std::array<D3D12_INPUT_ELEMENT_DESC, 5> inputLayout = {
        D3D12_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        D3D12_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geomDesc = {};
    geomDesc.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
    geomDesc.pRootSignature = mGeometryRootSignature.Get();
    geomDesc.VS = {vsGeom->GetBufferPointer(), vsGeom->GetBufferSize()};
    geomDesc.PS = {psGeom->GetBufferPointer(), psGeom->GetBufferSize()};
    geomDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geomDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    geomDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geomDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geomDesc.SampleMask = UINT_MAX;
    geomDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geomDesc.NumRenderTargets = 3;
    geomDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geomDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geomDesc.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
    geomDesc.DSVFormat = mDepthStencilFormat;
    geomDesc.SampleDesc.Count = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&geomDesc, IID_PPV_ARGS(&mGeometryPSO)),
                  "Create geometry PSO failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geomWireDesc = geomDesc;
    geomWireDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&geomWireDesc, IID_PPV_ARGS(&mGeometryWirePSO)),
                  "Create geometry wireframe PSO failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC tessDesc = geomDesc;
    tessDesc.VS = {vsCp->GetBufferPointer(), vsCp->GetBufferSize()};
    tessDesc.HS = {hs->GetBufferPointer(), hs->GetBufferSize()};
    tessDesc.DS = {ds->GetBufferPointer(), ds->GetBufferSize()};
    tessDesc.PS = {psGeom->GetBufferPointer(), psGeom->GetBufferSize()};
    tessDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&tessDesc, IID_PPV_ARGS(&mTessellationPSO)),
                  "Create tessellation PSO failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC tessWireDesc = tessDesc;
    tessWireDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&tessWireDesc, IID_PPV_ARGS(&mTessellationWirePSO)),
                  "Create tessellation wireframe PSO failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightDesc = {};
    lightDesc.InputLayout = {nullptr, 0};
    lightDesc.pRootSignature = mLightingRootSignature.Get();
    lightDesc.VS = {vsLighting->GetBufferPointer(), vsLighting->GetBufferSize()};
    lightDesc.PS = {psLighting->GetBufferPointer(), psLighting->GetBufferSize()};
    lightDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    lightDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    lightDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    lightDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    lightDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    lightDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    lightDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    lightDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    lightDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    lightDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    lightDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    lightDesc.DepthStencilState.DepthEnable = FALSE;
    lightDesc.DepthStencilState.StencilEnable = FALSE;

    lightDesc.SampleMask = UINT_MAX;
    lightDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightDesc.NumRenderTargets = 1;
    lightDesc.RTVFormats[0] = mBackBufferFormat;
    lightDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    lightDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&lightDesc, IID_PPV_ARGS(&mLightingPSO)),
                  "Create lighting PSO failed");
}
