#include "d3dApp.h"
#include "MathHelper.h"
#include "UploadBuffer.h"
#include <d3dx12.h>
#include <memory>
#include <utility>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

using Microsoft::WRL::ComPtr;

using namespace DirectX;

struct TgaImage
{
    UINT Width = 0;
    UINT Height = 0;

    std::vector<std::uint8_t> Pixels;
};

TgaImage LoadTgaFile(const std::wstring& filename)
{
    std::ifstream file(
        filename.c_str(),
        std::ios::binary);

    if (!file)
    {
        throw std::runtime_error(
            "Cannot open TGA texture file.");
    }

    std::uint8_t header[18] = {};

    file.read(
        reinterpret_cast<char*>(header),
        sizeof(header));

    if (!file)
    {
        throw std::runtime_error(
            "Cannot read TGA header.");
    }

    const UINT idLength = header[0];
    const UINT colorMapType = header[1];
    const UINT imageType = header[2];

    const UINT width =
        static_cast<UINT>(header[12]) |
        (static_cast<UINT>(header[13]) << 8);

    const UINT height =
        static_cast<UINT>(header[14]) |
        (static_cast<UINT>(header[15]) << 8);

    const UINT bitsPerPixel = header[16];

    if (width == 0 || height == 0)
    {
        throw std::runtime_error(
            "TGA texture has invalid size.");
    }

    if (colorMapType != 0)
    {
        throw std::runtime_error(
            "Color-mapped TGA is not supported.");
    }

    if (imageType != 2)
    {
        throw std::runtime_error(
            "Only uncompressed TGA is supported.");
    }

    if (bitsPerPixel != 24 && bitsPerPixel != 32)
    {
        throw std::runtime_error(
            "Only 24-bit and 32-bit TGA are supported.");
    }

    file.seekg(idLength, std::ios::cur);

    const UINT bytesPerPixel = bitsPerPixel / 8;

    const size_t sourceSize =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        bytesPerPixel;

    std::vector<std::uint8_t> sourcePixels(sourceSize);

    file.read(
        reinterpret_cast<char*>(sourcePixels.data()),
        sourceSize);

    if (!file)
    {
        throw std::runtime_error(
            "Cannot read TGA pixel data.");
    }

    TgaImage image;
    image.Width = width;
    image.Height = height;

    const size_t destinationSize =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        4;

    image.Pixels.resize(destinationSize);

    const bool topOrigin =
        (header[17] & 0x20) != 0;

    const bool rightOrigin =
        (header[17] & 0x10) != 0;

    for (UINT y = 0; y < height; ++y)
    {
        UINT sourceY =
            topOrigin ? y : height - 1 - y;

        for (UINT x = 0; x < width; ++x)
        {
            UINT sourceX =
                rightOrigin ? width - 1 - x : x;

            const size_t sourceIndex =
                (static_cast<size_t>(sourceY) * width +
                    sourceX) *
                bytesPerPixel;

            const size_t destinationIndex =
                (static_cast<size_t>(y) * width +
                    x) *
                4;

            image.Pixels[destinationIndex + 0] =
                sourcePixels[sourceIndex + 2];

            image.Pixels[destinationIndex + 1] =
                sourcePixels[sourceIndex + 1];

            image.Pixels[destinationIndex + 2] =
                sourcePixels[sourceIndex + 0];

            if (bitsPerPixel == 32)
            {
                image.Pixels[destinationIndex + 3] =
                    sourcePixels[sourceIndex + 3];
            }
            else
            {
                image.Pixels[destinationIndex + 3] = 255;
            }
        }
    }

    return image;
}

void CreateTextureFromTga(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    Texture& texture)
{
    TgaImage image =
        LoadTgaFile(texture.Filename);

    D3D12_RESOURCE_DESC textureDesc =
        CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            image.Width,
            image.Height,
            1,
            1);

    CD3DX12_HEAP_PROPERTIES defaultHeap(
        D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture.Resource)));

    UINT64 uploadBufferSize = 0;
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};

    device->GetCopyableFootprints(
        &textureDesc,
        0,
        1,
        0,
        &footprint,
        &numRows,
        &rowSizeInBytes,
        &uploadBufferSize);

    CD3DX12_HEAP_PROPERTIES uploadHeap(
        D3D12_HEAP_TYPE_UPLOAD);

    D3D12_RESOURCE_DESC uploadBufferDesc =
        CD3DX12_RESOURCE_DESC::Buffer(
            uploadBufferSize);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&texture.UploadHeap)));

    std::uint8_t* mappedData = nullptr;

    D3D12_RANGE readRange = {};
    readRange.Begin = 0;
    readRange.End = 0;

    ThrowIfFailed(
        texture.UploadHeap->Map(
            0,
            &readRange,
            reinterpret_cast<void**>(&mappedData)));

    const UINT sourceRowSize =
        image.Width * 4;

    for (UINT row = 0; row < image.Height; ++row)
    {
        std::memcpy(
            mappedData +
            footprint.Offset +
            static_cast<size_t>(row) *
            footprint.Footprint.RowPitch,

            image.Pixels.data() +
            static_cast<size_t>(row) *
            sourceRowSize,

            sourceRowSize);
    }

    texture.UploadHeap->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource =
        texture.Resource.Get();

    destination.Type =
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    destination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource =
        texture.UploadHeap.Get();

    source.Type =
        D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    source.PlacedFootprint = footprint;

    commandList->CopyTextureRegion(
        &destination,
        0,
        0,
        0,
        &source,
        nullptr);

    auto textureBarrier =
        CD3DX12_RESOURCE_BARRIER::Transition(
            texture.Resource.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    commandList->ResourceBarrier(
        1,
        &textureBarrier);
}

std::wstring MakeModelTexturePath(
    const std::string& relativePath,
    const std::wstring& fallback)
{
    if (relativePath.empty())
    {
        return fallback;
    }

    std::string normalizedPath = relativePath;

    std::replace(
        normalizedPath.begin(),
        normalizedPath.end(),
        '/',
        '\\');

    std::wstring result = L"Models\\";

    result += std::wstring(
        normalizedPath.begin(),
        normalizedPath.end());

    return result;
}

struct MaterialData
{
    std::string Name;
    std::wstring DiffuseFilename;
    std::wstring NormalFilename;
    std::wstring DisplacementFilename;
};

struct MaterialDraw
{
    int MaterialIndex = -1;

    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    UINT BaseVertexLocation = 0;
};

struct Vertex
{
    XMFLOAT3 Pos;

    XMFLOAT3 Normal;

    XMFLOAT2 TexC;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj =
        MathHelper::Identity4x4();

    XMFLOAT4X4 ViewProj =
        MathHelper::Identity4x4();

    XMFLOAT4X4 World =
        MathHelper::Identity4x4();

    XMFLOAT3 EyePosW =
        XMFLOAT3(0.0f, 0.0f, 0.0f);

    float DisplacementScale = 0.5f;

    XMFLOAT4 TessParams =
        XMFLOAT4(80.0f, 700.0f, 1.0f, 16.0f);

    XMFLOAT4 TexTransform =
        XMFLOAT4(
            1.0f,
            1.0f,
            0.0f,
            0.0f);
};

class BoxApp : public D3DApp
{
public:
    BoxApp(HINSTANCE hInstance);

    BoxApp(const BoxApp& rhs) = delete;
    BoxApp& operator=(const BoxApp& rhs) = delete;

    ~BoxApp();

    virtual bool Initialize() override;

private:
    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

    void BuildTexture();
    void BuildDescriptorHeaps();
    void BuildConstantBuffers();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildBoxGeometry();
    void BuildPSO();

private:
    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

    ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;

    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

    std::unique_ptr<MeshGeometry> mBoxGeo = nullptr;

    std::vector<MaterialData> mMaterials;

    std::vector<MaterialDraw> mDrawItems;

    std::vector<std::unique_ptr<Texture>> mTextures;

    UINT mCbvSrvUavDescriptorSize = 0;

    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mhsByteCode = nullptr;
    ComPtr<ID3DBlob> mdsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    ComPtr<ID3D12PipelineState> mPSO = nullptr;

    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    XMFLOAT3 mEyePos =
        XMFLOAT3(0.0f, 0.0f, 0.0f);

    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;

    float mRadius = 400.0f;
    XMFLOAT4 mTexTransform =
        XMFLOAT4(
            1.0f,
            1.0f,
            0.0f,
            0.0f);

    POINT mLastMousePos;
};

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE prevInstance,
    PSTR cmdLine,
    int showCmd)
{
#if defined(DEBUG) || defined(_DEBUG)
    _CrtSetDbgFlag(
        _CRTDBG_ALLOC_MEM_DF |
        _CRTDBG_LEAK_CHECK_DF
    );
#endif

    try
    {
        BoxApp theApp(hInstance);

        if (!theApp.Initialize())
        {
            return 0;
        }

        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(
            nullptr,
            e.ToString().c_str(),
            L"HR Failed",
            MB_OK
        );

        return 0;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(
            nullptr,
            e.what(),
            "Application error",
            MB_OK
        );

        return 0;
    }
}

BoxApp::BoxApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
}

BoxApp::~BoxApp()
{
}

bool BoxApp::Initialize()
{
    if (!D3DApp::Initialize())
    {
        return false;
    }

    ThrowIfFailed(
        mCommandList->Reset(
            mDirectCmdListAlloc.Get(),
            nullptr
        )
    );

    BuildBoxGeometry();
    BuildTexture();
    BuildDescriptorHeaps();
    BuildConstantBuffers();
    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildPSO();

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] =
    {
        mCommandList.Get()
    };

    mCommandQueue->ExecuteCommandLists(
        _countof(cmdsLists),
        cmdsLists
    );

    FlushCommandQueue();

    return true;
}

void BoxApp::OnResize()
{
    D3DApp::OnResize();

    XMMATRIX P =
        XMMatrixPerspectiveFovLH(
            0.25f * MathHelper::Pi,
            AspectRatio(),
            1.0f,
            100000.0f
        );

    XMStoreFloat4x4(&mProj, P);
}

void BoxApp::Update(const GameTimer& gt)
{
    float x =
        mRadius *
        sinf(mPhi) *
        cosf(mTheta);

    float z =
        mRadius *
        sinf(mPhi) *
        sinf(mTheta);

    float y =
        mRadius *
        cosf(mPhi);

    XMVECTOR pos =
        XMVectorSet(
            x,
            y,
            z,
            1.0f
        );

    XMStoreFloat3(
        &mEyePos,
        pos);

    XMVECTOR target =
        XMVectorZero();

    XMVECTOR up =
        XMVectorSet(
            0.0f,
            1.0f,
            0.0f,
            0.0f
        );

    XMMATRIX view =
        XMMatrixLookAtLH(
            pos,
            target,
            up
        );

    XMStoreFloat4x4(
        &mView,
        view
    );

    mTexTransform =
        XMFLOAT4(
            1.0f,
            1.0f,
            0.0f,
            0.0f);
}

void BoxApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(
        mDirectCmdListAlloc->Reset()
    );

    ThrowIfFailed(
        mCommandList->Reset(
            mDirectCmdListAlloc.Get(),
            mPSO.Get()
        )
    );

    mCommandList->RSSetViewports(
        1,
        &mScreenViewport
    );

    mCommandList->RSSetScissorRects(
        1,
        &mScissorRect
    );

    auto transition =
        CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );

    mCommandList->ResourceBarrier(
        1,
        &transition
    );

    mCommandList->ClearRenderTargetView(
        CurrentBackBufferView(),
        Colors::LightSteelBlue,
        0,
        nullptr
    );

    mCommandList->ClearDepthStencilView(
        DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH |
        D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr
    );

    auto cbbv =
        CurrentBackBufferView();

    auto dsv =
        DepthStencilView();

    mCommandList->OMSetRenderTargets(
        1,
        &cbbv,
        true,
        &dsv
    );

    ID3D12DescriptorHeap* descriptorHeaps[] =
    {
        mCbvHeap.Get()
    };

    mCommandList->SetDescriptorHeaps(
        _countof(descriptorHeaps),
        descriptorHeaps
    );

    mCommandList->SetGraphicsRootSignature(
        mRootSignature.Get()
    );

    XMMATRIX translate =
        XMMatrixTranslation(
            0.0f,
            50.0f,
            0.0f
        );

    XMMATRIX world =
        translate;

    XMMATRIX view =
        XMLoadFloat4x4(&mView);

    XMMATRIX proj =
        XMLoadFloat4x4(&mProj);

    XMMATRIX worldViewProj =
        world *
        view *
        proj;

    ObjectConstants objConstants;

    XMStoreFloat4x4(
        &objConstants.WorldViewProj,
        XMMatrixTranspose(worldViewProj)
    );

    XMStoreFloat4x4(
        &objConstants.World,
        XMMatrixTranspose(world)
    );

    XMStoreFloat4x4(
        &objConstants.ViewProj,
        XMMatrixTranspose(view * proj)
    );

    objConstants.EyePosW = mEyePos;
    objConstants.DisplacementScale = 2.0f;
    objConstants.TessParams =
        XMFLOAT4(
            80.0f,
            700.0f,
            1.0f,
            16.0f);

    objConstants.TexTransform =
        mTexTransform;

    mObjectCB->CopyData(
        0,
        objConstants
    );

    auto vbv =
        mBoxGeo->VertexBufferView();

    auto ibv =
        mBoxGeo->IndexBufferView();

    mCommandList->IASetVertexBuffers(
        0,
        1,
        &vbv
    );

    mCommandList->IASetIndexBuffer(
        &ibv
    );

    mCommandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
    );

    CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(
        mCbvHeap
        ->GetGPUDescriptorHandleForHeapStart());

    mCommandList->SetGraphicsRootDescriptorTable(
        0,
        cbvHandle);

    for (const MaterialDraw& drawItem : mDrawItems)
    {
        if (drawItem.MaterialIndex < 0)
        {
            continue;
        }

        if (
            static_cast<size_t>(
                drawItem.MaterialIndex)
            >= mMaterials.size())
        {
            continue;
        }

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
            mCbvHeap
            ->GetGPUDescriptorHandleForHeapStart());

        srvHandle.Offset(
            1 + drawItem.MaterialIndex * 3,
            mCbvSrvUavDescriptorSize);

        mCommandList->SetGraphicsRootDescriptorTable(
            1,
            srvHandle);

        mCommandList->DrawIndexedInstanced(
            drawItem.IndexCount,
            1,
            drawItem.StartIndexLocation,
            drawItem.BaseVertexLocation,
            0);
    }

    transition =
        CD3DX12_RESOURCE_BARRIER::Transition(
            CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT
        );

    mCommandList->ResourceBarrier(
        1,
        &transition
    );

    ThrowIfFailed(
        mCommandList->Close()
    );

    ID3D12CommandList* cmdsLists[] =
    {
        mCommandList.Get()
    };

    mCommandQueue->ExecuteCommandLists(
        _countof(cmdsLists),
        cmdsLists
    );

    ThrowIfFailed(
        mSwapChain->Present(
            0,
            0
        )
    );

    mCurrBackBuffer =
        (mCurrBackBuffer + 1) %
        SwapChainBufferCount;

    FlushCommandQueue();
}

void BoxApp::OnMouseDown(
    WPARAM btnState,
    int x,
    int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void BoxApp::OnMouseUp(
    WPARAM btnState,
    int x,
    int y)
{
    ReleaseCapture();
}

void BoxApp::OnMouseMove(
    WPARAM btnState,
    int x,
    int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        float dx =
            XMConvertToRadians(
                0.25f *
                static_cast<float>(
                    x - mLastMousePos.x
                    )
            );

        float dy =
            XMConvertToRadians(
                0.25f *
                static_cast<float>(
                    y - mLastMousePos.y
                    )
            );

        mTheta += dx;
        mPhi += dy;

        mPhi =
            MathHelper::Clamp(
                mPhi,
                0.1f,
                MathHelper::Pi - 0.1f
            );
    }
    else if ((btnState & MK_RBUTTON) != 0)
    {
        float dx =
            0.005f *
            static_cast<float>(
                x - mLastMousePos.x
                );

        float dy =
            0.005f *
            static_cast<float>(
                y - mLastMousePos.y
                );

        mRadius += dx - dy;

        mRadius =
            MathHelper::Clamp(
                mRadius,
                3.0f,
                1000.0f
            );
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void BoxApp::BuildTexture()
{
    mTextures.clear();

    for (const MaterialData& material : mMaterials)
    {
        const std::wstring filenames[] =
        {
            material.DiffuseFilename,
            material.NormalFilename,
            material.DisplacementFilename
        };

        const char* names[] =
        {
            "_Diffuse",
            "_Normal",
            "_Displacement"
        };

        for (int textureIndex = 0;
            textureIndex < 3;
            ++textureIndex)
        {
            auto texture =
                std::make_unique<Texture>();

            texture->Name =
                "Sponza_" +
                material.Name +
                names[textureIndex];

            texture->Filename =
                filenames[textureIndex];

            CreateTextureFromTga(
                md3dDevice.Get(),
                mCommandList.Get(),
                *texture);

            mTextures.push_back(
                std::move(texture));
        }
    }
}

void BoxApp::BuildDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};

    cbvHeapDesc.NumDescriptors =
        1 + static_cast<UINT>(
            mTextures.size());
    cbvHeapDesc.Type =
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    cbvHeapDesc.Flags =
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    cbvHeapDesc.NodeMask = 0;

    mCbvSrvUavDescriptorSize =
        md3dDevice->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(
        md3dDevice->CreateDescriptorHeap(
            &cbvHeapDesc,
            IID_PPV_ARGS(&mCbvHeap)));
}

void BoxApp::BuildConstantBuffers()
{
    mObjectCB =
        std::make_unique<
        UploadBuffer<ObjectConstants>>(
            md3dDevice.Get(),
            1,
            true);

    UINT objCBByteSize =
        d3dUtil::CalcConstantBufferByteSize(
            sizeof(ObjectConstants));

    D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
        mObjectCB->Resource()
        ->GetGPUVirtualAddress();

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};

    cbvDesc.BufferLocation =
        cbAddress;

    cbvDesc.SizeInBytes =
        objCBByteSize;

    md3dDevice->CreateConstantBufferView(
        &cbvDesc,
        mCbvHeap
        ->GetCPUDescriptorHandleForHeapStart());

    for (
        UINT textureIndex = 0;
        textureIndex <
        static_cast<UINT>(mTextures.size());
        ++textureIndex)
    {
        D3D12_RESOURCE_DESC textureDesc =
            mTextures[textureIndex]
            ->Resource
            ->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

        srvDesc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        srvDesc.Format =
            textureDesc.Format;

        srvDesc.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;

        srvDesc.Texture2D.MostDetailedMip =
            0;

        srvDesc.Texture2D.MipLevels =
            textureDesc.MipLevels;

        srvDesc.Texture2D.ResourceMinLODClamp =
            0.0f;

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
            mCbvHeap
            ->GetCPUDescriptorHandleForHeapStart());

        srvHandle.Offset(
            static_cast<INT>(textureIndex + 1),
            mCbvSrvUavDescriptorSize);

        md3dDevice->CreateShaderResourceView(
            mTextures[textureIndex]
            ->Resource
            .Get(),

            &srvDesc,
            srvHandle);
    }
}

void BoxApp::BuildRootSignature()
{
    CD3DX12_ROOT_PARAMETER slotRootParameter[2];

    CD3DX12_DESCRIPTOR_RANGE cbvTable;

    cbvTable.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        1,
        0);

    CD3DX12_DESCRIPTOR_RANGE srvTable;

    srvTable.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        3,
        0);

    slotRootParameter[0].InitAsDescriptorTable(
        1,
        &cbvTable);

    slotRootParameter[1].InitAsDescriptorTable(
        1,
        &srvTable);

    CD3DX12_STATIC_SAMPLER_DESC sampler(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        2,
        slotRootParameter,
        1,
        &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;

    HRESULT hr =
        D3D12SerializeRootSignature(
            &rootSigDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(),
            errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA(
            static_cast<char*>(
                errorBlob->GetBufferPointer()));
    }

    ThrowIfFailed(hr);

    ThrowIfFailed(
        md3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(&mRootSignature)));
}
void BoxApp::BuildShadersAndInputLayout()
{
    mvsByteCode =
        d3dUtil::CompileShader(
            L"Shaders\\tessellation.hlsl",
            nullptr,
            "VS",
            "vs_5_0");

    mhsByteCode =
        d3dUtil::CompileShader(
            L"Shaders\\tessellation.hlsl",
            nullptr,
            "HS",
            "hs_5_0");

    mdsByteCode =
        d3dUtil::CompileShader(
            L"Shaders\\tessellation.hlsl",
            nullptr,
            "DS",
            "ds_5_0");

    mpsByteCode =
        d3dUtil::CompileShader(
            L"Shaders\\tessellation.hlsl",
            nullptr,
            "PS",
            "ps_5_0");

    mInputLayout =
    {
        {
            "POSITION",
            0,
            DXGI_FORMAT_R32G32B32_FLOAT,
            0,
            0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            0
        },

        {
            "NORMAL",
            0,
            DXGI_FORMAT_R32G32B32_FLOAT,
            0,
            12,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            0
        },

        {
            "TEXCOORD",
            0,
            DXGI_FORMAT_R32G32_FLOAT,
            0,
            24,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            0
        }
    };
}

void BoxApp::BuildBoxGeometry()
{
    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile("Models/sponza.obj"))
    {
        if (!reader.Error().empty())
        {
            OutputDebugStringA(
                reader.Error().c_str());
        }

        throw std::runtime_error(
            "Failed to load OBJ.");
    }

    const tinyobj::attrib_t& attrib =
        reader.GetAttrib();

    const std::vector<tinyobj::shape_t>& shapes =
        reader.GetShapes();

    const std::vector<tinyobj::material_t>& objMaterials =
        reader.GetMaterials();

    mMaterials.clear();

    const std::wstring fallbackTexture =
        L"Models\\textures\\sponza_arch_diff.tga";

    const std::wstring fallbackNormal =
        L"Models\\textures\\sponza_arch_ddn.tga";

    for (size_t i = 0; i < objMaterials.size(); ++i)
    {
        const tinyobj::material_t& objMaterial =
            objMaterials[i];

        MaterialData material;

        material.Name =
            objMaterial.name;

        if (material.Name.empty())
        {
            material.Name =
                "Material_" +
                std::to_string(i);
        }

        material.DiffuseFilename =
            MakeModelTexturePath(
                objMaterial.diffuse_texname,
                fallbackTexture);

        std::string normalTextureName =
            objMaterial.normal_texname;

        if (normalTextureName.empty())
        {
            normalTextureName =
                objMaterial.bump_texname;
        }

        material.NormalFilename =
            MakeModelTexturePath(
                normalTextureName,
                fallbackNormal);

        material.DisplacementFilename =
            MakeModelTexturePath(
                objMaterial.displacement_texname,
                material.NormalFilename);

        mMaterials.push_back(
            material);
    }

    if (mMaterials.empty())
    {
        MaterialData defaultMaterial;

        defaultMaterial.Name =
            "DefaultMaterial";

        defaultMaterial.DiffuseFilename =
            fallbackTexture;

        defaultMaterial.NormalFilename =
            fallbackNormal;

        defaultMaterial.DisplacementFilename =
            fallbackNormal;

        mMaterials.push_back(
            defaultMaterial);
    }

    DirectX::XMFLOAT3 bmin(
        +FLT_MAX,
        +FLT_MAX,
        +FLT_MAX);

    DirectX::XMFLOAT3 bmax(
        -FLT_MAX,
        -FLT_MAX,
        -FLT_MAX);

    for (size_t i = 0;
        i + 2 < attrib.vertices.size();
        i += 3)
    {
        float x =
            attrib.vertices[i + 0];

        float y =
            attrib.vertices[i + 1];

        float z =
            attrib.vertices[i + 2];

        bmin.x =
            (std::min)(bmin.x, x);

        bmin.y =
            (std::min)(bmin.y, y);

        bmin.z =
            (std::min)(bmin.z, z);

        bmax.x =
            (std::max)(bmax.x, x);

        bmax.y =
            (std::max)(bmax.y, y);

        bmax.z =
            (std::max)(bmax.z, z);
    }

    XMFLOAT3 center(
        0.5f * (bmin.x + bmax.x),
        0.5f * (bmin.y + bmax.y),
        0.5f * (bmin.z + bmax.z));

    std::vector<Vertex> vertices;

    std::vector<std::vector<std::uint32_t>>
        materialIndices(
            mMaterials.size());

    for (const auto& shape : shapes)
    {
        size_t indexOffset = 0;

        for (
            size_t face = 0;
            face < shape.mesh.num_face_vertices.size();
            ++face)
        {
            unsigned int faceVertexCount =
                shape.mesh.num_face_vertices[face];

            int materialIndex = -1;

            if (face < shape.mesh.material_ids.size())
            {
                materialIndex =
                    shape.mesh.material_ids[face];
            }

            if (materialIndex < 0 ||
                materialIndex >=
                static_cast<int>(mMaterials.size()))
            {
                materialIndex = 0;
            }

            for (
                unsigned int vertexInFace = 0;
                vertexInFace < faceVertexCount;
                ++vertexInFace)
            {
                const tinyobj::index_t& index =
                    shape.mesh.indices[
                        indexOffset + vertexInFace];

                if (index.vertex_index < 0)
                {
                    continue;
                }

                Vertex vertex = {};

                const size_t positionIndex =
                    static_cast<size_t>(
                        3 * index.vertex_index);

                vertex.Pos =
                    XMFLOAT3(
                        attrib.vertices[positionIndex + 0]
                        - center.x,

                        attrib.vertices[positionIndex + 1]
                        - center.y,

                        attrib.vertices[positionIndex + 2]
                        - center.z);

                vertex.Normal =
                    XMFLOAT3(
                        0.0f,
                        1.0f,
                        0.0f);

                if (index.normal_index >= 0)
                {
                    const size_t normalIndex =
                        static_cast<size_t>(
                            3 * index.normal_index);

                    if (normalIndex + 2 <
                        attrib.normals.size())
                    {
                        vertex.Normal =
                            XMFLOAT3(
                                attrib.normals[
                                    normalIndex + 0],

                                    attrib.normals[
                                        normalIndex + 1],

                                        attrib.normals[
                                            normalIndex + 2]);
                    }
                }

                vertex.TexC =
                    XMFLOAT2(
                        0.0f,
                        0.0f);

                if (index.texcoord_index >= 0)
                {
                    const size_t texcoordIndex =
                        static_cast<size_t>(
                            2 * index.texcoord_index);

                    if (texcoordIndex + 1 <
                        attrib.texcoords.size())
                    {
                        vertex.TexC =
                            XMFLOAT2(
                                attrib.texcoords[
                                    texcoordIndex + 0],

                                    1.0f -
                                    attrib.texcoords[
                                        texcoordIndex + 1]);
                    }
                }

                vertices.push_back(vertex);

                materialIndices[
                    materialIndex].push_back(
                        static_cast<std::uint32_t>(
                            vertices.size() - 1));
            }

            indexOffset += faceVertexCount;
        }
    }

    std::vector<std::uint32_t> indices;

    mDrawItems.clear();

    for (
        size_t materialIndex = 0;
        materialIndex < materialIndices.size();
        ++materialIndex)
    {
        if (materialIndices[materialIndex].empty())
        {
            continue;
        }

        MaterialDraw drawItem;

        drawItem.MaterialIndex =
            static_cast<int>(materialIndex);

        drawItem.StartIndexLocation =
            static_cast<UINT>(
                indices.size());

        drawItem.IndexCount =
            static_cast<UINT>(
                materialIndices[materialIndex].size());

        drawItem.BaseVertexLocation = 0;

        indices.insert(
            indices.end(),
            materialIndices[materialIndex].begin(),
            materialIndices[materialIndex].end());

        mDrawItems.push_back(
            drawItem);
    }

    const UINT vbByteSize =
        static_cast<UINT>(
            vertices.size() *
            sizeof(Vertex));

    const UINT ibByteSize =
        static_cast<UINT>(
            indices.size() *
            sizeof(std::uint32_t));

    mBoxGeo =
        std::make_unique<MeshGeometry>();

    mBoxGeo->Name =
        "Sponza";

    ThrowIfFailed(
        D3DCreateBlob(
            vbByteSize,
            &mBoxGeo->VertexBufferCPU));

    CopyMemory(
        mBoxGeo->VertexBufferCPU
        ->GetBufferPointer(),

        vertices.data(),
        vbByteSize);

    ThrowIfFailed(
        D3DCreateBlob(
            ibByteSize,
            &mBoxGeo->IndexBufferCPU));

    CopyMemory(
        mBoxGeo->IndexBufferCPU
        ->GetBufferPointer(),

        indices.data(),
        ibByteSize);

    mBoxGeo->VertexBufferGPU =
        d3dUtil::CreateDefaultBuffer(
            md3dDevice.Get(),
            mCommandList.Get(),
            vertices.data(),
            vbByteSize,
            mBoxGeo->VertexBufferUploader);

    mBoxGeo->IndexBufferGPU =
        d3dUtil::CreateDefaultBuffer(
            md3dDevice.Get(),
            mCommandList.Get(),
            indices.data(),
            ibByteSize,
            mBoxGeo->IndexBufferUploader);

    mBoxGeo->VertexByteStride =
        sizeof(Vertex);

    mBoxGeo->VertexBufferByteSize =
        vbByteSize;

    mBoxGeo->IndexFormat =
        DXGI_FORMAT_R32_UINT;

    mBoxGeo->IndexBufferByteSize =
        ibByteSize;
}

void BoxApp::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;

    ZeroMemory(
        &psoDesc,
        sizeof(
            D3D12_GRAPHICS_PIPELINE_STATE_DESC
            )
    );

    psoDesc.InputLayout =
    {
        mInputLayout.data(),
        static_cast<UINT>(
            mInputLayout.size()
        )
    };

    psoDesc.pRootSignature =
        mRootSignature.Get();

    psoDesc.VS =
    {
        reinterpret_cast<BYTE*>(
            mvsByteCode->GetBufferPointer()
        ),
        mvsByteCode->GetBufferSize()
    };

    psoDesc.HS =
    {
        reinterpret_cast<BYTE*>(
            mhsByteCode->GetBufferPointer()
        ),
        mhsByteCode->GetBufferSize()
    };

    psoDesc.DS =
    {
        reinterpret_cast<BYTE*>(
            mdsByteCode->GetBufferPointer()
        ),
        mdsByteCode->GetBufferSize()
    };

    psoDesc.PS =
    {
        reinterpret_cast<BYTE*>(
            mpsByteCode->GetBufferPointer()
        ),
        mpsByteCode->GetBufferSize()
    };

    psoDesc.RasterizerState =
        CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    psoDesc.BlendState =
        CD3DX12_BLEND_DESC(
            D3D12_DEFAULT
        );

    psoDesc.DepthStencilState =
        CD3DX12_DEPTH_STENCIL_DESC(
            D3D12_DEFAULT
        );

    psoDesc.SampleMask =
        UINT_MAX;

    psoDesc.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;

    psoDesc.NumRenderTargets =
        1;

    psoDesc.RTVFormats[0] =
        mBackBufferFormat;

    psoDesc.SampleDesc.Count =
        m4xMsaaState ? 4 : 1;

    psoDesc.SampleDesc.Quality =
        m4xMsaaState
        ? m4xMsaaQuality - 1
        : 0;

    psoDesc.DSVFormat =
        mDepthStencilFormat;

    ThrowIfFailed(
        md3dDevice->CreateGraphicsPipelineState(
            &psoDesc,
            IID_PPV_ARGS(&mPSO)
        )
    );
}
