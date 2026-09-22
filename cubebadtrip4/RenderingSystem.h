#pragma once

#include "Gbuffer.h"

#include <DirectXMath.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <vector>

enum class DeferredLightType
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct DeferredLight
{
    DirectX::XMFLOAT4 PositionRange;
    DirectX::XMFLOAT4 DirectionIntensity;
    DirectX::XMFLOAT4 ColorType;
    DirectX::XMFLOAT4 SpotParams;
};

class RenderingSystem
{
public:
    bool Initialize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        DXGI_FORMAT backBufferFormat);

    void Resize(UINT width, UINT height);

    void BeginGeometryPass(
        ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView);

    void RenderLighting(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect);

    bool IsInitialized() const { return mInitialized; }
    Gbuffer& GetGBuffer() { return mGbuffer; }
    std::vector<DeferredLight>& GetLights() { return mLights; }

private:
    void BuildRootSignature();
    void BuildPipelineStates(DXGI_FORMAT backBufferFormat);
    void BuildDefaultLights();
    void DrawLight(
        ID3D12GraphicsCommandList* commandList,
        const DeferredLight& light);

private:
    Microsoft::WRL::ComPtr<ID3D12Device> mDevice;
    Gbuffer mGbuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mAmbientPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mAdditivePso;
    Microsoft::WRL::ComPtr<ID3DBlob> mVertexShader;
    Microsoft::WRL::ComPtr<ID3DBlob> mPixelShader;

    std::vector<DeferredLight> mLights;
    bool mInitialized = false;
};
