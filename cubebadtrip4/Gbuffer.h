#pragma once

#include <array>
#include <wrl.h>
#include <d3d12.h>

class Gbuffer
{
public:
    static constexpr UINT TargetCount = 3;

    bool Initialize(ID3D12Device* device, UINT width, UINT height);
    void Resize(UINT width, UINT height);

    void BeginGeometryPass(
        ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView);

    void EndGeometryPass(ID3D12GraphicsCommandList* commandList);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(UINT index) const;
    ID3D12DescriptorHeap* GetSrvHeap() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvTableStart() const;

    UINT GetWidth() const { return mWidth; }
    UINT GetHeight() const { return mHeight; }
    bool IsInitialized() const { return mDevice != nullptr; }

private:
    void CreateDescriptorHeaps();
    void CreateResources();

private:
    Microsoft::WRL::ComPtr<ID3D12Device> mDevice;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, TargetCount> mTargets;

    UINT mRtvDescriptorSize = 0;
    UINT mSrvDescriptorSize = 0;
    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mInGeometryPass = false;
};
