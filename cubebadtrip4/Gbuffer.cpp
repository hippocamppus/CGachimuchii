#include "Gbuffer.h"

#include "d3dUtil.h"
#include "d3dx12.h"

#include <algorithm>

namespace
{
    const DXGI_FORMAT GBufferFormats[Gbuffer::TargetCount] =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM,       
        DXGI_FORMAT_R16G16B16A16_FLOAT,  
        DXGI_FORMAT_R32G32B32A32_FLOAT    
    };

    const float GBufferClearColors[Gbuffer::TargetCount][4] =
    {
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f }
    };
}

bool Gbuffer::Initialize(ID3D12Device* device, UINT width, UINT height)
{
    if (device == nullptr)
    {
        return false;
    }

    mDevice = device;
    mRtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateDescriptorHeaps();
    Resize(width, height);
    return true;
}

void Gbuffer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = TargetCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ThrowIfFailed(mDevice->CreateDescriptorHeap(
        &rtvHeapDesc,
        IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = TargetCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ThrowIfFailed(mDevice->CreateDescriptorHeap(
        &srvHeapDesc,
        IID_PPV_ARGS(mSrvHeap.GetAddressOf())));
}

void Gbuffer::Resize(UINT width, UINT height)
{
    if (mDevice == nullptr || mRtvHeap == nullptr || mSrvHeap == nullptr)
    {
        return;
    }

    mWidth = (std::max)(1u, width);
    mHeight = (std::max)(1u, height);
    mInGeometryPass = false;

    for (auto& target : mTargets)
    {
        target.Reset();
    }

    CreateResources();
}

void Gbuffer::CreateResources()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart());

    for (UINT i = 0; i < TargetCount; ++i)
    {
        const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            GBufferFormats[i],
            mWidth,
            mHeight,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

        D3D12_CLEAR_VALUE optimizedClearValue = {};
        optimizedClearValue.Format = GBufferFormats[i];
        for (UINT channel = 0; channel < 4; ++channel)
        {
            optimizedClearValue.Color[channel] = GBufferClearColors[i][channel];
        }

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &optimizedClearValue,
            IID_PPV_ARGS(mTargets[i].GetAddressOf())));

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = GBufferFormats[i];
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        rtvDesc.Texture2D.PlaneSlice = 0;
        mDevice->CreateRenderTargetView(mTargets[i].Get(), &rtvDesc, rtvHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = GBufferFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        mDevice->CreateShaderResourceView(mTargets[i].Get(), &srvDesc, srvHandle);

        rtvHandle.Offset(1, mRtvDescriptorSize);
        srvHandle.Offset(1, mSrvDescriptorSize);
    }
}

void Gbuffer::BeginGeometryPass(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView)
{
    if (commandList == nullptr || !IsInitialized() || mInGeometryPass)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[TargetCount] = {};
    for (UINT i = 0; i < TargetCount; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mTargets[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    commandList->ResourceBarrier(TargetCount, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[TargetCount];
    for (UINT i = 0; i < TargetCount; ++i)
    {
        rtvs[i] = GetRtv(i);
        commandList->ClearRenderTargetView(
            rtvs[i],
            GBufferClearColors[i],
            0,
            nullptr);
    }

    commandList->ClearDepthStencilView(
        depthStencilView,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr);

    commandList->OMSetRenderTargets(TargetCount, rtvs, FALSE, &depthStencilView);
    mInGeometryPass = true;
}

void Gbuffer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr || !mInGeometryPass)
    {
        return;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    D3D12_RESOURCE_BARRIER barriers[TargetCount] = {};
    for (UINT i = 0; i < TargetCount; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mTargets[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    commandList->ResourceBarrier(TargetCount, barriers);
    mInGeometryPass = false;
}

D3D12_CPU_DESCRIPTOR_HANDLE Gbuffer::GetRtv(UINT index) const
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(index, mRtvDescriptorSize);
    return handle;
}

ID3D12DescriptorHeap* Gbuffer::GetSrvHeap() const
{
    return mSrvHeap.Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE Gbuffer::GetSrvTableStart() const
{
    return mSrvHeap->GetGPUDescriptorHandleForHeapStart();
}
