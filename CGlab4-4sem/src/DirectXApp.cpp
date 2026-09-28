#include "DirectXApp.h"

#include "DDSTextureLoader.h"
#include "GameTimer.h"
#include "d3dUtil.h"
#include "model_loader.h"
#include "../h/d3dx12.h"

#include <Windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <unordered_set>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace {
std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void AppendMeshData(MeshData& dst, const MeshData& src) {
    const unsigned int vertexOffset = static_cast<unsigned int>(dst.vertices.size());
    const unsigned int indexOffset = static_cast<unsigned int>(dst.indices.size());

    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());

    dst.indices.reserve(dst.indices.size() + src.indices.size());
    for (unsigned int idx : src.indices) {
        dst.indices.push_back(idx + vertexOffset);
    }

    dst.submeshes.reserve(dst.submeshes.size() + src.submeshes.size());
    for (auto sm : src.submeshes) {
        sm.startIndexLocation += indexOffset;
        sm.baseVertexLocation = 0;
        dst.submeshes.push_back(sm);
    }
}

template<typename TObject>
BoundingBox MakeBoundsForObjects(const std::vector<TObject>& objects,
                                 const std::vector<unsigned int>& indices) {
    XMFLOAT3 vMin(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    XMFLOAT3 vMax(
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max());

    for (unsigned int index : indices) {
        const auto& sphere = objects[index].bounds;
        vMin.x = (std::min)(vMin.x, sphere.Center.x - sphere.Radius);
        vMin.y = (std::min)(vMin.y, sphere.Center.y - sphere.Radius);
        vMin.z = (std::min)(vMin.z, sphere.Center.z - sphere.Radius);
        vMax.x = (std::max)(vMax.x, sphere.Center.x + sphere.Radius);
        vMax.y = (std::max)(vMax.y, sphere.Center.y + sphere.Radius);
        vMax.z = (std::max)(vMax.z, sphere.Center.z + sphere.Radius);
    }

    BoundingBox box;
    BoundingBox::CreateFromPoints(box, XMLoadFloat3(&vMin), XMLoadFloat3(&vMax));
    return box;
}

bool SphereFitsInBox(const BoundingSphere& sphere, const BoundingBox& box) {
    const XMFLOAT3 min(
        box.Center.x - box.Extents.x,
        box.Center.y - box.Extents.y,
        box.Center.z - box.Extents.z);
    const XMFLOAT3 max(
        box.Center.x + box.Extents.x,
        box.Center.y + box.Extents.y,
        box.Center.z + box.Extents.z);

    return sphere.Center.x - sphere.Radius >= min.x &&
           sphere.Center.y - sphere.Radius >= min.y &&
           sphere.Center.z - sphere.Radius >= min.z &&
           sphere.Center.x + sphere.Radius <= max.x &&
           sphere.Center.y + sphere.Radius <= max.y &&
           sphere.Center.z + sphere.Radius <= max.z;
}

ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer) {
    ComPtr<ID3D12Resource> defaultBuffer;

    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(byteSize),
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&defaultBuffer)),
        "Create default buffer failed");

    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(byteSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)),
        "Create upload buffer failed");

    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData = initData;
    subData.RowPitch = byteSize;
    subData.SlicePitch = subData.RowPitch;

    auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        defaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1, &toCopyDest);

    UpdateSubresources<1>(cmdList, defaultBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &subData);

    auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
        defaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdList->ResourceBarrier(1, &toRead);

    return defaultBuffer;
}

bool LoadWicTextureFromFile12(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filePath,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& uploadHeap) {
    static ComPtr<IWICImagingFactory> wicFactory;
    if (!wicFactory) {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr)) {
            hr = CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wicFactory));
        }
        if (FAILED(hr)) {
            return false;
        }
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory->CreateDecoderFromFilename(
        filePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory->CreateFormatConverter(&converter))) {
        return false;
    }

    if (FAILED(converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT rowPitch = width * 4u;
    const UINT imageSize = rowPitch * height;
    std::vector<unsigned char> pixels(imageSize);

    if (FAILED(converter->CopyPixels(nullptr, rowPitch, imageSize, pixels.data()))) {
        return false;
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)))) {
        return false;
    }

    const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);
    if (FAILED(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadHeap)))) {
        return false;
    }

    D3D12_SUBRESOURCE_DATA subresource = {};
    subresource.pData = pixels.data();
    subresource.RowPitch = rowPitch;
    subresource.SlicePitch = imageSize;

    UpdateSubresources(cmdList, texture.Get(), uploadHeap.Get(), 0, 0, 1, &subresource);

    auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSrv);

    return true;
}

}

DirectXApp::DirectXApp() = default;

DirectXApp::~DirectXApp() {
    if (mDevice) {
        FlushCommandQueue();
    }
    if (mComInitialized) {
        CoUninitialize();
        mComInitialized = false;
    }
}

bool DirectXApp::Initialize(HWND hwnd, unsigned int width, unsigned int height) {
    mHwnd = hwnd;
    mClientWidth = width;
    mClientHeight = height;

    if (!InitDirect3D()) {
        return false;
    }

    mRenderingSystem = std::make_unique<RenderingSystem>();
    if (!mRenderingSystem->Initialize(mDevice.Get(), mClientWidth, mClientHeight, mBackBufferFormat, mDepthStencilFormat)) {
        return false;
    }

    BuildScene();

    mScreenViewport.TopLeftX = 0.0f;
    mScreenViewport.TopLeftY = 0.0f;
    mScreenViewport.Width = static_cast<float>(mClientWidth);
    mScreenViewport.Height = static_cast<float>(mClientHeight);
    mScreenViewport.MinDepth = 0.0f;
    mScreenViewport.MaxDepth = 1.0f;

    mScissorRect = {0, 0, static_cast<LONG>(mClientWidth), static_cast<LONG>(mClientHeight)};

    return true;
}

bool DirectXApp::InitDirect3D() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comHr)) {
        mComInitialized = true;
    } else if (comHr != RPC_E_CHANGED_MODE) {
        ThrowIfFailed(comHr, "CoInitializeEx failed");
    }

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&mDxgiFactory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> hardwareAdapter;
    for (UINT adapterIndex = 0;
         mDxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(&hardwareAdapter)) != DXGI_ERROR_NOT_FOUND;
         ++adapterIndex) {
        DXGI_ADAPTER_DESC1 desc = {};
        hardwareAdapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
            break;
        }
        hardwareAdapter.Reset();
    }

    if (!hardwareAdapter) {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(mDxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)), "EnumWarpAdapter failed");
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice)),
                      "Create WARP device failed");
    } else {
        ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice)),
                      "Create hardware device failed");
    }

    ThrowIfFailed(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), "Create fence failed");

    mRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDsvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCbvSrvUavDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateCommandObjects();
    CreateSwapChain();
    CreateRtvAndDsvDescriptorHeaps();
    CreateRenderTargetViews();
    CreateDepthStencilBuffer();

    return true;
}

void DirectXApp::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&mCommandQueue)), "Create command queue failed");

    ThrowIfFailed(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mDirectCmdListAlloc)),
                  "Create command allocator failed");

    ThrowIfFailed(mDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mDirectCmdListAlloc.Get(),
        nullptr,
        IID_PPV_ARGS(&mCommandList)),
        "Create command list failed");

    ThrowIfFailed(mCommandList->Close(), "Close command list failed");
}

void DirectXApp::CreateSwapChain() {
    mSwapChain.Reset();

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = mClientWidth;
    sd.BufferDesc.Height = mClientHeight;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = mBackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = SwapChainBufferCount;
    sd.OutputWindow = mHwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ThrowIfFailed(mDxgiFactory->CreateSwapChain(mCommandQueue.Get(), &sd, &mSwapChain),
                  "Create swap chain failed");
}

void DirectXApp::CreateRtvAndDsvDescriptorHeaps() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)), "Create RTV heap failed");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap)), "Create DSV heap failed");
}

void DirectXApp::CreateRenderTargetViews() {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (unsigned int i = 0; i < SwapChainBufferCount; ++i) {
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i])), "Get swap chain buffer failed");
        mDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, mRtvDescriptorSize);
    }
}

void DirectXApp::CreateDepthStencilBuffer() {
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = mClientWidth;
    depthDesc.Height = mClientHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = mDepthStencilFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = mDepthStencilFormat;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear,
        IID_PPV_ARGS(&mDepthStencilBuffer)),
        "Create depth buffer failed");

    mDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), nullptr, DepthStencilView());
}

void DirectXApp::OnResize(unsigned int width, unsigned int height) {
    if (!mDevice || !mSwapChain) {
        return;
    }

    mClientWidth = (std::max)(1u, width);
    mClientHeight = (std::max)(1u, height);

    FlushCommandQueue();

    for (auto& buffer : mSwapChainBuffer) {
        buffer.Reset();
    }
    mDepthStencilBuffer.Reset();

    ThrowIfFailed(mSwapChain->ResizeBuffers(
        SwapChainBufferCount,
        mClientWidth,
        mClientHeight,
        mBackBufferFormat,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH),
        "ResizeBuffers failed");

    mCurrBackBuffer = 0;

    CreateRenderTargetViews();
    CreateDepthStencilBuffer();

    if (mRenderingSystem) {
        mRenderingSystem->OnResize(mDevice.Get(), mClientWidth, mClientHeight);
    }

    mScreenViewport.TopLeftX = 0.0f;
    mScreenViewport.TopLeftY = 0.0f;
    mScreenViewport.Width = static_cast<float>(mClientWidth);
    mScreenViewport.Height = static_cast<float>(mClientHeight);
    mScreenViewport.MinDepth = 0.0f;
    mScreenViewport.MaxDepth = 1.0f;
    mScissorRect = {0, 0, static_cast<LONG>(mClientWidth), static_cast<LONG>(mClientHeight)};

    if (mObjectCB && mPassCB && mLightingCB) {
        BuildMainSrvHeap();
    }
}

void DirectXApp::BuildScene() {
    ThrowIfFailed(mDirectCmdListAlloc->Reset(), "Reset command allocator failed");
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr), "Reset command list failed");

    LoadModels();
    BuildSceneObjects();
    BuildGeometryBuffers();
    LoadTextures();
    CreateFallbackTextures();

    BuildConstantBuffers();
    BuildMainSrvHeap();
    BindSubmeshTextures();
    BuildLights();

    ThrowIfFailed(mCommandList->Close(), "Close command list failed");
    ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1, cmdLists);
    FlushCommandQueue();

    mVertexBufferUploader.Reset();
    mIndexBufferUploader.Reset();
}

void DirectXApp::LoadModels() {
    mSceneMesh = {};

    std::filesystem::path modelPath = "../assets/Earth.fbx";
    if (!std::filesystem::exists(modelPath)) {
        modelPath = "../assets/earth.fbx";
    }
    if (!std::filesystem::exists(modelPath)) {
        mEyePos = XMFLOAT3(0.0f, 8.0f, -170.0f);
        mYaw = 0.0f;
        mPitch = -0.18f;
        mModelBounds = BoundingSphere(XMFLOAT3(0.0f, 0.0f, 0.0f), 1.0f);
        return;
    }

    auto mesh = ModelLoader::LoadModel(
        modelPath.u8string(),
        XMMatrixIdentity());

    XMFLOAT3 vMin(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    XMFLOAT3 vMax(
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max());

    for (const auto& v : mesh.vertices) {
        vMin.x = (std::min)(vMin.x, v.Position.x);
        vMin.y = (std::min)(vMin.y, v.Position.y);
        vMin.z = (std::min)(vMin.z, v.Position.z);

        vMax.x = (std::max)(vMax.x, v.Position.x);
        vMax.y = (std::max)(vMax.y, v.Position.y);
        vMax.z = (std::max)(vMax.z, v.Position.z);
    }

    const XMFLOAT3 center(
        0.5f * (vMin.x + vMax.x),
        0.5f * (vMin.y + vMax.y),
        0.5f * (vMin.z + vMax.z));

    float radius = 0.0f;
    for (const auto& v : mesh.vertices) {
        const float dx = v.Position.x - center.x;
        const float dy = v.Position.y - center.y;
        const float dz = v.Position.z - center.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        radius = (std::max)(radius, d);
    }

    const float targetRadius = 2.4f;
    const float normalizeScale = (radius > 1e-4f) ? (targetRadius / radius) : 1.0f;

    for (auto& v : mesh.vertices) {
        v.Position.x = (v.Position.x - center.x) * normalizeScale;
        v.Position.y = (v.Position.y - center.y) * normalizeScale + targetRadius * 0.9f;
        v.Position.z = (v.Position.z - center.z) * normalizeScale;
    }

    mModelBounds = BoundingSphere(XMFLOAT3(0.0f, targetRadius * 0.9f, 0.0f), targetRadius);

    for (auto& sm : mesh.submeshes) {
        sm.material.diffuseTextureName = "Earth_ALB";
        sm.material.normalTextureName = "Earth_NORM";
        sm.material.displacementTextureName.clear();
        sm.material.shininess = 64.0f;
    }

    AppendMeshData(mSceneMesh, mesh);

    std::string msg = "Loaded model: " + modelPath.u8string() + "\n";
    OutputDebugStringA(msg.c_str());
    msg = "Mesh stats: vertices=" + std::to_string(mSceneMesh.vertices.size()) +
          " indices=" + std::to_string(mSceneMesh.indices.size()) +
          " submeshes=" + std::to_string(mSceneMesh.submeshes.size()) + "\n";
    OutputDebugStringA(msg.c_str());

    mEyePos = XMFLOAT3(0.0f, 8.0f, -170.0f);
    mYaw = 0.0f;
    mPitch = -0.04f;
}

void DirectXApp::BuildSceneObjects() {
    mSceneObjects.clear();
    mVisibleObjectIndices.clear();
    mOctreeNodes.clear();

    if (mSceneMesh.vertices.empty()) {
        return;
    }

    std::mt19937 rng{202604u};
    std::uniform_real_distribution<float> jitter(-1.0f, 1.0f);
    std::uniform_real_distribution<float> scaleDist(0.55f, 1.15f);
    std::uniform_real_distribution<float> rotationDist(0.0f, XM_2PI);

    const unsigned int side = static_cast<unsigned int>(std::ceil(std::sqrt(static_cast<float>(SceneObjectCount))));
    const float spacing = 5.8f;
    const float halfSide = 0.5f * static_cast<float>(side - 1);

    mSceneObjects.reserve(SceneObjectCount);
    for (unsigned int i = 0; i < SceneObjectCount; ++i) {
        const unsigned int x = i % side;
        const unsigned int z = i / side;
        const float scale = scaleDist(rng);

        XMFLOAT3 position(
            (static_cast<float>(x) - halfSide) * spacing + jitter(rng),
            0.0f,
            (static_cast<float>(z) - halfSide) * spacing + jitter(rng));

        const XMMATRIX world =
            XMMatrixScaling(scale, scale, scale) *
            XMMatrixRotationY(rotationDist(rng)) *
            XMMatrixTranslation(position.x, position.y, position.z);

        SceneObject object;
        XMStoreFloat4x4(&object.world, world);

        XMVECTOR localCenter = XMLoadFloat3(&mModelBounds.Center);
        XMFLOAT3 worldCenter;
        XMStoreFloat3(&worldCenter, XMVector3TransformCoord(localCenter, world));
        object.bounds = BoundingSphere(worldCenter, mModelBounds.Radius * scale);

        mSceneObjects.push_back(object);
    }

    BuildOctree();
}

void DirectXApp::BuildOctree() {
    mOctreeNodes.clear();
    if (mSceneObjects.empty()) {
        return;
    }

    std::vector<unsigned int> allIndices(mSceneObjects.size());
    for (unsigned int i = 0; i < allIndices.size(); ++i) {
        allIndices[i] = i;
    }

    const BoundingBox rootBounds = MakeBoundsForObjects(mSceneObjects, allIndices);

    auto buildNode = [&](auto&& self,
                         const BoundingBox& bounds,
                         const std::vector<unsigned int>& indices,
                         unsigned int depth) -> int {
        const int nodeIndex = static_cast<int>(mOctreeNodes.size());
        mOctreeNodes.push_back(OctreeNode{});
        mOctreeNodes[nodeIndex].bounds = bounds;

        if (depth >= OctreeMaxDepth || indices.size() <= OctreeLeafCapacity) {
            mOctreeNodes[nodeIndex].objectIndices = indices;
            return nodeIndex;
        }

        const XMFLOAT3 childExtents(
            bounds.Extents.x * 0.5f,
            bounds.Extents.y * 0.5f,
            bounds.Extents.z * 0.5f);

        std::array<BoundingBox, 8> childBounds;
        std::array<std::vector<unsigned int>, 8> childObjects;
        for (unsigned int child = 0; child < 8; ++child) {
            const float sx = (child & 1u) ? 1.0f : -1.0f;
            const float sy = (child & 2u) ? 1.0f : -1.0f;
            const float sz = (child & 4u) ? 1.0f : -1.0f;
            childBounds[child] = BoundingBox(
                XMFLOAT3(bounds.Center.x + sx * childExtents.x,
                         bounds.Center.y + sy * childExtents.y,
                         bounds.Center.z + sz * childExtents.z),
                childExtents);
        }

        for (unsigned int objectIndex : indices) {
            int containingChild = -1;
            for (unsigned int child = 0; child < 8; ++child) {
                if (SphereFitsInBox(mSceneObjects[objectIndex].bounds, childBounds[child])) {
                    containingChild = static_cast<int>(child);
                    break;
                }
            }

            if (containingChild >= 0) {
                childObjects[static_cast<unsigned int>(containingChild)].push_back(objectIndex);
            } else {
                mOctreeNodes[nodeIndex].objectIndices.push_back(objectIndex);
            }
        }

        for (unsigned int child = 0; child < 8; ++child) {
            if (!childObjects[child].empty()) {
                mOctreeNodes[nodeIndex].children[child] =
                    self(self, childBounds[child], childObjects[child], depth + 1);
            }
        }

        return nodeIndex;
    };

    buildNode(buildNode, rootBounds, allIndices, 0);
}

void DirectXApp::QueryOctree(const BoundingFrustum& frustum, std::vector<unsigned int>& visible) const {
    if (mOctreeNodes.empty()) {
        return;
    }

    auto queryNode = [&](auto&& self, int nodeIndex) -> void {
        const OctreeNode& node = mOctreeNodes[static_cast<unsigned int>(nodeIndex)];
        if (frustum.Contains(node.bounds) == DISJOINT) {
            return;
        }

        for (unsigned int objectIndex : node.objectIndices) {
            if (IsObjectVisible(objectIndex, frustum)) {
                visible.push_back(objectIndex);
            }
        }

        for (int childIndex : node.children) {
            if (childIndex >= 0) {
                self(self, childIndex);
            }
        }
    };

    queryNode(queryNode, 0);
}

bool DirectXApp::IsObjectVisible(unsigned int objectIndex, const BoundingFrustum& frustum) const {
    return frustum.Contains(mSceneObjects[objectIndex].bounds) != DISJOINT;
}

void DirectXApp::BuildGeometryBuffers() {
    const UINT vbByteSize = static_cast<UINT>(mSceneMesh.vertices.size() * sizeof(Vertex));
    const UINT ibByteSize = static_cast<UINT>(mSceneMesh.indices.size() * sizeof(unsigned int));

    mVertexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(), mSceneMesh.vertices.data(), vbByteSize,
                                           mVertexBufferUploader);

    mIndexBufferGPU = CreateDefaultBuffer(mDevice.Get(), mCommandList.Get(), mSceneMesh.indices.data(), ibByteSize,
                                          mIndexBufferUploader);

    mVertexBufferView.BufferLocation = mVertexBufferGPU->GetGPUVirtualAddress();
    mVertexBufferView.StrideInBytes = sizeof(Vertex);
    mVertexBufferView.SizeInBytes = vbByteSize;

    mIndexBufferView.BufferLocation = mIndexBufferGPU->GetGPUVirtualAddress();
    mIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    mIndexBufferView.SizeInBytes = ibByteSize;
}

void DirectXApp::LoadTextures() {
    mTextureResources.clear();
    mTextureNameToIndex.clear();

    const std::array<std::wstring, 3> dirs = {
        L"../assets/textures/sponza",
        L"../assets/textures/earth",
        L"../assets/textures/Rails"
    };

    for (const auto& dir : dirs) {
        if (!std::filesystem::exists(dir)) {
            continue;
        }

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const auto ext = ToLowerAscii(entry.path().extension().string());
            const bool isDDS = (ext == ".dds");
            const bool isWIC = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tif" || ext == ".tiff");
            if (!isDDS && !isWIC) {
                continue;
            }

            TextureResource tex;
            tex.path = entry.path().wstring();

            bool loaded = false;
            if (isDDS) {
                const HRESULT hr = DirectX::CreateDDSTextureFromFile12(
                    mDevice.Get(),
                    mCommandList.Get(),
                    tex.path.c_str(),
                    tex.resource,
                    tex.uploadHeap);
                loaded = SUCCEEDED(hr);
            } else {
                loaded = LoadWicTextureFromFile12(
                    mDevice.Get(),
                    mCommandList.Get(),
                    tex.path,
                    tex.resource,
                    tex.uploadHeap);
            }

            if (!loaded) {
                std::string msg = "Failed to load texture: " + entry.path().string() + "\n";
                OutputDebugStringA(msg.c_str());
                continue;
            }

            const std::string name = ToLowerAscii(entry.path().stem().string());
            if (mTextureNameToIndex.find(name) != mTextureNameToIndex.end()) {
                continue;
            }

            const unsigned int newIndex = static_cast<unsigned int>(mTextureResources.size());
            mTextureNameToIndex[name] = newIndex;
            mTextureResources.push_back(std::move(tex));
        }
    }
}

void DirectXApp::CreateFallbackTextures() {
    auto addSolid = [&](const std::string& key, unsigned int rgba) {
        TextureResource tex;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&tex.resource)),
            "Create fallback texture failed");

        const UINT64 uploadSize = GetRequiredIntermediateSize(tex.resource.Get(), 0, 1);

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&tex.uploadHeap)),
            "Create fallback upload failed");

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = &rgba;
        subresource.RowPitch = 4;
        subresource.SlicePitch = 4;

        UpdateSubresources(mCommandList.Get(), tex.resource.Get(), tex.uploadHeap.Get(), 0, 0, 1, &subresource);
        auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            tex.resource.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        mCommandList->ResourceBarrier(1, &toSrv);

        const unsigned int newIndex = static_cast<unsigned int>(mTextureResources.size());
        mTextureNameToIndex[key] = newIndex;
        mTextureResources.push_back(std::move(tex));

        return newIndex;
    };

    mFallbackDiffuseIndex = addSolid("__fallback_diffuse", 0xFFFFFFFFu);
    mFallbackNormalIndex = addSolid("__fallback_normal", 0xFFFF8080u);
    mFallbackDisplacementIndex = addSolid("__fallback_displacement", 0xFF000000u);
}

void DirectXApp::BuildConstantBuffers() {
    const unsigned int objectCountPerView = (std::max)(
        1u,
        static_cast<unsigned int>(mSceneObjects.size() * mSceneMesh.submeshes.size()));
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(mDevice.Get(), objectCountPerView * 2u, true);
    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(mDevice.Get(), 2, true);
    mLightingCB = std::make_unique<UploadBuffer<LightingConstants>>(mDevice.Get(), LightingCbElementCount, true);
}

void DirectXApp::BuildMainSrvHeap() {
    const unsigned int textureCount = static_cast<unsigned int>(mTextureResources.size());
    const unsigned int descriptorCount = 3 + textureCount + GBuffer::Count;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = descriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mCbvSrvHeap)), "Create SRV heap failed");

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = mObjectCB->Resource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    mDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
    cpuHandle.Offset(1, mCbvSrvUavDescriptorSize);

    cbvDesc.BufferLocation = mPassCB->Resource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
    mDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
    cpuHandle.Offset(1, mCbvSrvUavDescriptorSize);

    cbvDesc.BufferLocation = mLightingCB->Resource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = d3dUtil::CalcConstantBufferByteSize(sizeof(LightingConstants));
    mDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
    cpuHandle.Offset(1, mCbvSrvUavDescriptorSize);

    mTextureSrvStart = 3;

    for (unsigned int i = 0; i < textureCount; ++i) {
        auto& tex = mTextureResources[i];

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = tex.resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = tex.resource->GetDesc().MipLevels;
        srvDesc.Texture2D.PlaneSlice = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        mDevice->CreateShaderResourceView(tex.resource.Get(), &srvDesc, cpuHandle);
        tex.srvHeapIndex = mTextureSrvStart + i;
        cpuHandle.Offset(1, mCbvSrvUavDescriptorSize);
    }

    mGBufferSrvStart = mTextureSrvStart + textureCount;

    CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferCpu(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                                             static_cast<INT>(mGBufferSrvStart),
                                             mCbvSrvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE gbufferGpu(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
                                             static_cast<INT>(mGBufferSrvStart),
                                             mCbvSrvUavDescriptorSize);

    mRenderingSystem->GetGBuffer()->CreateSrvs(
        mDevice.Get(),
        gbufferCpu,
        gbufferGpu,
        mCbvSrvUavDescriptorSize);
}

void DirectXApp::BindSubmeshTextures() {
    auto resolve = [&](const std::string& rawName, unsigned int fallbackIndex) -> unsigned int {
        const std::string key = ToLowerAscii(rawName);
        auto it = mTextureNameToIndex.find(key);
        if (it == mTextureNameToIndex.end()) {
            return mTextureResources[fallbackIndex].srvHeapIndex;
        }
        return mTextureResources[it->second].srvHeapIndex;
    };

    for (auto& submesh : mSceneMesh.submeshes) {
        submesh.material.diffuseSrvHeapIndex = resolve(submesh.material.diffuseTextureName, mFallbackDiffuseIndex);
        submesh.material.normalSrvHeapIndex = resolve(submesh.material.normalTextureName, mFallbackNormalIndex);

        if (submesh.material.displacementTextureName.empty()) {
            submesh.material.displacementSrvHeapIndex = mTextureResources[mFallbackDisplacementIndex].srvHeapIndex;
        } else {
            submesh.material.displacementSrvHeapIndex =
                resolve(submesh.material.displacementTextureName, mFallbackDisplacementIndex);
        }
    }
}

void DirectXApp::BuildLights() {
    mLights.clear();

    LightData dir;
    dir.Type = static_cast<unsigned int>(LightType::Directional);
    dir.Direction = XMFLOAT3(-0.25f, -1.0f, 0.35f);
    dir.Color = XMFLOAT3(1.0f, 0.97f, 0.92f);
    dir.Intensity = 1.1f;
    mLights.push_back(dir);

    auto addPoint = [&](const XMFLOAT3& pos, const XMFLOAT3& color, float intensity, float range) {
        LightData point;
        point.Type = static_cast<unsigned int>(LightType::Point);
        point.Position = pos;
        point.Color = color;
        point.Intensity = intensity;
        point.Range = range;
        mLights.push_back(point);
    };

    addPoint(XMFLOAT3(-10.0f, 5.0f, -2.0f), XMFLOAT3(1.0f, 0.35f, 0.35f), 5.5f, 26.0f);
    addPoint(XMFLOAT3(9.0f, 6.0f, 7.0f), XMFLOAT3(0.3f, 0.55f, 1.0f), 4.8f, 24.0f);
    addPoint(XMFLOAT3(0.0f, 10.0f, -11.0f), XMFLOAT3(0.45f, 1.0f, 0.45f), 3.8f, 28.0f);

    auto addSpot = [&](const XMFLOAT3& pos, const XMFLOAT3& dirVec, const XMFLOAT3& color, float intensity, float range, float angle) {
        LightData spot;
        spot.Type = static_cast<unsigned int>(LightType::Spot);
        spot.Position = pos;
        spot.Direction = dirVec;
        spot.Color = color;
        spot.Intensity = intensity;
        spot.Range = range;
        spot.SpotAngle = angle;
        mLights.push_back(spot);
    };

    addSpot(XMFLOAT3(13.0f, 9.0f, 0.0f), XMFLOAT3(-1.0f, -0.75f, 0.0f), XMFLOAT3(1.0f, 0.9f, 0.5f), 2.2f, 34.0f, 0.35f);
    addSpot(XMFLOAT3(-13.0f, 8.0f, 3.0f), XMFLOAT3(1.0f, -0.85f, -0.15f), XMFLOAT3(0.4f, 1.0f, 0.9f), 2.0f, 30.0f, 0.32f);
}

void DirectXApp::UpdateFallingLights(float dt) {
    if (!mFallingBallsEnabled) {
        mFallingLights.clear();
        return;
    }

    static std::mt19937 rng{1337u};
    std::uniform_real_distribution<float> xzDist(-14.0f, 14.0f);
    std::uniform_real_distribution<float> colorDist(0.4f, 1.0f);
    std::uniform_real_distribution<float> speedDist(1.2f, 2.6f);
    std::uniform_real_distribution<float> rangeDist(6.0f, 12.0f);

    mFallingSpawnTimer += dt;
    while (mFallingSpawnTimer >= mFallingSpawnInterval && mFallingLights.size() < mMaxFallingLights) {
        mFallingSpawnTimer -= mFallingSpawnInterval;

        FallingLight light;
        light.position = XMFLOAT3(xzDist(rng), 12.0f + speedDist(rng) * 2.0f, xzDist(rng));
        light.velocity = XMFLOAT3(0.0f, -(3.5f + speedDist(rng)), 0.0f);
        light.color = XMFLOAT3(colorDist(rng), colorDist(rng), colorDist(rng));
        light.intensity = 6.5f + speedDist(rng) * 2.0f;
        light.range = rangeDist(rng);
        light.settled = false;
        mFallingLights.push_back(light);
    }

    constexpr float kGroundY = -0.02f;
    constexpr float kGravity = 8.5f;

    for (auto& light : mFallingLights) {
        if (!light.settled) {
            light.velocity.y -= kGravity * dt;
            light.position.y += light.velocity.y * dt;
            if (light.position.y <= kGroundY) {
                light.position.y = kGroundY;
                light.velocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
                light.settled = true;
            }
        } else {
            light.intensity -= 1.2f * dt;
            light.range -= 0.7f * dt;
        }
    }

    mFallingLights.erase(
        std::remove_if(mFallingLights.begin(), mFallingLights.end(), [](const FallingLight& light) {
            return light.intensity <= 0.2f || light.range <= 0.8f;
        }),
        mFallingLights.end());
}

void DirectXApp::UpdateCamera(float dt) {
    const float moveSpeed = 5.f;

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::cos(mPitch) * std::sin(mYaw),
        std::sin(mPitch),
        std::cos(mPitch) * std::cos(mYaw),
        0.0f));

    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));

    XMVECTOR position = XMLoadFloat3(&mEyePos);

    if (GetAsyncKeyState('W') & 0x8000) {
        position += forward * moveSpeed * dt;
    }
    if (GetAsyncKeyState('S') & 0x8000) {
        position -= forward * moveSpeed * dt;
    }
    if (GetAsyncKeyState('A') & 0x8000) {
        position -= right * moveSpeed * dt;
    }
    if (GetAsyncKeyState('D') & 0x8000) {
        position += right * moveSpeed * dt;
    }

    XMStoreFloat3(&mEyePos, position);
}

void DirectXApp::Update(const GameTimer& gt) {
    UpdateCamera(gt.DeltaTime());

    const bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    const bool f2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    const bool bDown = (GetAsyncKeyState('B') & 0x8000) != 0;
    const bool tDown = (GetAsyncKeyState('T') & 0x8000) != 0;
    const bool rDown = (GetAsyncKeyState('R') & 0x8000) != 0;
    const bool cDown = (GetAsyncKeyState('C') & 0x8000) != 0;
    const bool oDown = (GetAsyncKeyState('O') & 0x8000) != 0;
    bool titleDirty = false;

    if (f1Down && !mF1WasDown) {
        mDebugViewMode = 1;
    }
    if (f2Down && !mF2WasDown) {
        mDebugViewMode = 2;
    }
    if (f3Down && !mF3WasDown) {
        mDebugViewMode = 3;
        titleDirty = true;
    }

    mF1WasDown = f1Down;
    mF2WasDown = f2Down;
    mF3WasDown = f3Down;

    if (bDown && !mBWasDown) {
        mFallingBallsEnabled = !mFallingBallsEnabled;
        if (!mFallingBallsEnabled) {
            mFallingLights.clear();
        }
        titleDirty = true;
    }
    mBWasDown = bDown;

    if (tDown && !mTWasDown) {
        mAnimateTextures = !mAnimateTextures;
        titleDirty = true;
    }
    mTWasDown = tDown;

    if (rDown && !mRWasDown) {
        mAnimateTextures = false;
        mTexAnimU = 0.0f;
        mTexAnimV = 0.0f;
        mTexScaleU = 1.0f;
        mTexScaleV = 1.0f;
        titleDirty = true;
    }
    mRWasDown = rDown;

    if (cDown && !mCWasDown) {
        mFrustumCullingEnabled = !mFrustumCullingEnabled;
        titleDirty = true;
    }
    mCWasDown = cDown;

    if (oDown && !mOWasDown) {
        mOctreeCullingEnabled = !mOctreeCullingEnabled;
        titleDirty = true;
    }
    mOWasDown = oDown;

    auto clampTiling = [](float v) {
        return std::clamp(v, 0.10f, 16.0f);
    };

    const float tileRate = 1.2f;
    float deltaU = 0.0f;
    float deltaV = 0.0f;

    if (GetAsyncKeyState('Y') & 0x8000) {
        deltaU += tileRate * gt.DeltaTime();
        deltaV += tileRate * gt.DeltaTime();
    }
    if (GetAsyncKeyState('H') & 0x8000) {
        deltaU -= tileRate * gt.DeltaTime();
        deltaV -= tileRate * gt.DeltaTime();
    }
    if (GetAsyncKeyState('U') & 0x8000) {
        deltaU += tileRate * gt.DeltaTime();
    }
    if (GetAsyncKeyState('J') & 0x8000) {
        deltaU -= tileRate * gt.DeltaTime();
    }
    if (GetAsyncKeyState('I') & 0x8000) {
        deltaV += tileRate * gt.DeltaTime();
    }
    if (GetAsyncKeyState('K') & 0x8000) {
        deltaV -= tileRate * gt.DeltaTime();
    }

    if (std::abs(deltaU) > 0.0f || std::abs(deltaV) > 0.0f) {
        mTexScaleU = clampTiling(mTexScaleU + deltaU);
        mTexScaleV = clampTiling(mTexScaleV + deltaV);
        titleDirty = true;
    }

    if (mDebugViewMode != mLastTitleMode) {
        titleDirty = true;
    }

    if (titleDirty) {
        std::wostringstream ws;
        ws << L"DirectX 12 Framework";
        if (mDebugViewMode == 2) {
            ws << L" | F2: Normal Debug";
        } else if (mDebugViewMode == 3) {
            ws << L" | F3: Tess Debug + Wireframe";
        } else {
            ws << L" | F1: Default";
        }

        ws << L" | B: Falling Lights " << (mFallingBallsEnabled ? L"ON" : L"OFF");
        ws << L" | Culling " << (mFrustumCullingEnabled ? L"ON" : L"OFF");
        ws << L" | Octree " << (mOctreeCullingEnabled ? L"ON" : L"OFF");
        ws << L" | Visible " << mLastVisibleObjectCount << L"/" << mSceneObjects.size();
        ws << L" | Draws " << mLastDrawCallCount;
        ws << L" | T: Texture/Water Anim " << (mAnimateTextures ? L"ON" : L"OFF");
        ws << std::fixed << std::setprecision(2);
        ws << L" | TileU=" << mTexScaleU << L" TileV=" << mTexScaleV;
        SetWindowTextW(mHwnd, ws.str().c_str());
        mLastTitleMode = mDebugViewMode;
    }

    UpdateFallingLights(gt.DeltaTime());

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::cos(mPitch) * std::sin(mYaw),
        std::sin(mPitch),
        std::cos(mPitch) * std::cos(mYaw),
        0.0f));

    const XMVECTOR eye = XMLoadFloat3(&mEyePos);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI,
                                                   static_cast<float>(mClientWidth) / static_cast<float>(mClientHeight),
                                                   CameraNearZ,
                                                   CameraFarZ);
    if (mAnimateTextures) {
        mTexAnimU += 0.04f * gt.DeltaTime();
        mTexAnimV += 0.015f * gt.DeltaTime();
        if (mTexAnimU > 1.0f) {
            mTexAnimU -= 1.0f;
        }
        if (mTexAnimV > 1.0f) {
            mTexAnimV -= 1.0f;
        }
    }
    PassConstants pass = {};
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
    XMStoreFloat4x4(&pass.InvViewProj, XMMatrixTranspose(invViewProj));
    pass.EyePosW = mEyePos;
    pass.AmbientColor = XMFLOAT4(0.08f, 0.08f, 0.1f, 1.0f);
    mPassCB->CopyData(0, pass);
}

void DirectXApp::Draw(const GameTimer& gt) {
    if (mSceneMesh.submeshes.empty() || mSceneMesh.indices.empty() || mSceneMesh.vertices.empty()) {
        OutputDebugStringA("Draw skipped: mesh is empty.\n");
        return;
    }
    FlushCommandQueue();

    ThrowIfFailed(mDirectCmdListAlloc->Reset(), "Reset command allocator failed");
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr), "Reset command list failed");

    ID3D12DescriptorHeap* descriptorHeaps[] = {mCbvSrvHeap.Get()};
    mCommandList->SetDescriptorHeaps(1, descriptorHeaps);

    auto* gbuffer = mRenderingSystem->GetGBuffer();
    D3D12_CPU_DESCRIPTOR_HANDLE gbuffRtvs[3] = {
        gbuffer->GetRtv(GBuffer::Albedo),
        gbuffer->GetRtv(GBuffer::Normal),
        gbuffer->GetRtv(GBuffer::Depth)
    };

    const auto transitionGBuffer = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        std::array<D3D12_RESOURCE_BARRIER, GBuffer::Count> barriers{};
        for (unsigned int i = 0; i < GBuffer::Count; ++i) {
            barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                gbuffer->GetTexture(static_cast<GBuffer::TextureType>(i)),
                before,
                after);
        }
        mCommandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    };

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::cos(mPitch) * std::sin(mYaw),
        std::sin(mPitch),
        std::cos(mPitch) * std::cos(mYaw),
        0.0f));
    const XMVECTOR eye = XMLoadFloat3(&mEyePos);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI,
                                                   static_cast<float>(mClientWidth) / static_cast<float>(mClientHeight),
                                                   CameraNearZ,
                                                   CameraFarZ);

    BoundingFrustum cameraFrustum;
    BoundingFrustum::CreateFromMatrix(cameraFrustum, proj);
    BoundingFrustum worldFrustum;
    cameraFrustum.Transform(worldFrustum, XMMatrixInverse(nullptr, view));

    mVisibleObjectIndices.clear();
    if (!mFrustumCullingEnabled) {
        mVisibleObjectIndices.resize(mSceneObjects.size());
        for (unsigned int i = 0; i < mVisibleObjectIndices.size(); ++i) {
            mVisibleObjectIndices[i] = i;
        }
    } else if (mOctreeCullingEnabled) {
        QueryOctree(worldFrustum, mVisibleObjectIndices);
    } else {
        for (unsigned int i = 0; i < mSceneObjects.size(); ++i) {
            if (IsObjectVisible(i, worldFrustum)) {
                mVisibleObjectIndices.push_back(i);
            }
        }
    }

    if (mVisibleObjectIndices.size() > MaxRenderedObjectCount) {
        const XMFLOAT3 eyePos = mEyePos;
        std::sort(mVisibleObjectIndices.begin(), mVisibleObjectIndices.end(), [&](unsigned int lhs, unsigned int rhs) {
            const XMFLOAT3& a = mSceneObjects[lhs].bounds.Center;
            const XMFLOAT3& b = mSceneObjects[rhs].bounds.Center;
            const float da =
                (a.x - eyePos.x) * (a.x - eyePos.x) +
                (a.y - eyePos.y) * (a.y - eyePos.y) +
                (a.z - eyePos.z) * (a.z - eyePos.z);
            const float db =
                (b.x - eyePos.x) * (b.x - eyePos.x) +
                (b.y - eyePos.y) * (b.y - eyePos.y) +
                (b.z - eyePos.z) * (b.z - eyePos.z);
            return da < db;
        });
        mVisibleObjectIndices.resize(MaxRenderedObjectCount);
    }

    mLastVisibleObjectCount = static_cast<unsigned int>(mVisibleObjectIndices.size());
    mLastDrawCallCount = mLastVisibleObjectCount * static_cast<unsigned int>(mSceneMesh.submeshes.size());

    const unsigned int objectElementSize = mObjectCB->GetElementSize();
    const unsigned int passElementSize = mPassCB->GetElementSize();
    const unsigned int objectCountPerView = (std::max)(
        1u,
        static_cast<unsigned int>(mSceneObjects.size() * mSceneMesh.submeshes.size()));
    const auto passCbAddress = [&](unsigned int passIndex) -> D3D12_GPU_VIRTUAL_ADDRESS {
        return mPassCB->Resource()->GetGPUVirtualAddress() + static_cast<UINT64>(passIndex) * passElementSize;
    };
    const auto objectCbAddress = [&](unsigned int objectCbIndex) -> D3D12_GPU_VIRTUAL_ADDRESS {
        return mObjectCB->Resource()->GetGPUVirtualAddress() + static_cast<UINT64>(objectCbIndex) * objectElementSize;
    };

    const auto writePassConstants = [&](unsigned int passIndex,
                                        const XMMATRIX& passView,
                                        const XMMATRIX& passProj,
                                        const XMFLOAT3& eyePos) {
        PassConstants pass = {};
        XMMATRIX invViewProj = XMMatrixInverse(nullptr, passView * passProj);
        XMStoreFloat4x4(&pass.InvViewProj, XMMatrixTranspose(invViewProj));
        pass.EyePosW = eyePos;
        pass.AmbientColor = XMFLOAT4(0.08f, 0.08f, 0.1f, 1.0f);
        mPassCB->CopyData(static_cast<int>(passIndex), pass);
    };

    const auto renderGeometryPass = [&](const D3D12_VIEWPORT& viewport,
                                        const D3D12_RECT& scissor,
                                        const XMMATRIX& passView,
                                        const XMMATRIX& passProj,
                                        unsigned int passIndex,
                                        unsigned int objectCbOffset) {
        mCommandList->RSSetViewports(1, &viewport);
        mCommandList->RSSetScissorRects(1, &scissor);

        gbuffer->Clear(mCommandList.Get());
        mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                            1.0f, 0, 0, nullptr);

        auto dsv = DepthStencilView();
        mCommandList->OMSetRenderTargets(3, gbuffRtvs, FALSE, &dsv);
        mCommandList->IASetVertexBuffers(0, 1, &mVertexBufferView);
        mCommandList->IASetIndexBuffer(&mIndexBufferView);

        mCommandList->SetGraphicsRootSignature(mRenderingSystem->GetGeometryRootSignature());
        mCommandList->SetGraphicsRootConstantBufferView(1, passCbAddress(passIndex));

        unsigned int objectCbIndex = objectCbOffset;

        for (unsigned int sceneObjectIndex : mVisibleObjectIndices) {
            const auto& sceneObject = mSceneObjects[sceneObjectIndex];
            const XMMATRIX world = XMLoadFloat4x4(&sceneObject.world);

            for (const auto& submesh : mSceneMesh.submeshes) {
                const bool hasDisplacement = !submesh.material.displacementTextureName.empty() &&
                                             submesh.material.displacementSrvHeapIndex !=
                                                 mTextureResources[mFallbackDisplacementIndex].srvHeapIndex;
                const bool tessellated = hasDisplacement;
                const bool wireframeDebug = (mDebugViewMode == 3);
                const XMMATRIX texTransform =
                    XMMatrixScaling(mTexScaleU, mTexScaleV, 1.0f) * XMMatrixTranslation(mTexAnimU, mTexAnimV, 0.0f);

                ObjectConstants obj = {};
                XMStoreFloat4x4(&obj.World, XMMatrixTranspose(world));
                XMStoreFloat4x4(&obj.WorldViewProj, XMMatrixTranspose(world * passView * passProj));
                XMStoreFloat4x4(&obj.TextureTransform, XMMatrixTranspose(texTransform));
                obj.TotalTime = mAnimateTextures ? gt.TotalTime() : 0.0f;
                obj.Params.x = static_cast<float>(mDebugViewMode);
                obj.Params.y = 0.085f;
                obj.Params.z = 0.0f;
                obj.Params.w = 0.0f;
                mObjectCB->CopyData(static_cast<int>(objectCbIndex), obj);
                mCommandList->SetGraphicsRootConstantBufferView(0, objectCbAddress(objectCbIndex));

                if (tessellated) {
                    mCommandList->SetPipelineState(wireframeDebug
                                                       ? mRenderingSystem->GetTessellationWirePSO()
                                                       : mRenderingSystem->GetTessellationPSO());
                    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
                } else {
                    mCommandList->SetPipelineState(wireframeDebug
                                                       ? mRenderingSystem->GetGeometryWirePSO()
                                                       : mRenderingSystem->GetGeometryPSO());
                    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                }

                mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrvHandle(submesh.material.diffuseSrvHeapIndex));
                mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrvHandle(submesh.material.normalSrvHeapIndex));
                mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrvHandle(submesh.material.displacementSrvHeapIndex));

                mCommandList->DrawIndexedInstanced(
                    submesh.indexCount,
                    1,
                    submesh.startIndexLocation,
                    submesh.baseVertexLocation,
                    0);
                ++objectCbIndex;
            }
        }
    };

    const unsigned int lightElementSize = mLightingCB->GetElementSize();
    const auto lightingCbAddress = [&](unsigned int index) -> D3D12_GPU_VIRTUAL_ADDRESS {
        return mLightingCB->Resource()->GetGPUVirtualAddress() + static_cast<UINT64>(index) * lightElementSize;
    };

    const auto renderLightingPass = [&](const D3D12_VIEWPORT& viewport,
                                        const D3D12_RECT& scissor,
                                        unsigned int passIndex,
                                        bool clearBackBuffer) {
        mCommandList->RSSetViewports(1, &viewport);
        mCommandList->RSSetScissorRects(1, &scissor);

        auto rtv = CurrentBackBufferView();
        const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (clearBackBuffer) {
            mCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        } else {
            mCommandList->ClearRenderTargetView(rtv, clearColor, 1, &scissor);
        }
        mCommandList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);

        mCommandList->SetPipelineState(mRenderingSystem->GetLightingPSO());
        mCommandList->SetGraphicsRootSignature(mRenderingSystem->GetLightingRootSignature());
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        mCommandList->SetGraphicsRootDescriptorTable(0, GetGpuSrvHandle(mGBufferSrvStart));
        mCommandList->SetGraphicsRootConstantBufferView(1, passCbAddress(passIndex));

        unsigned int lightCbIndex = 0;

        LightingConstants ambientConst = {};
        ambientConst.EnableAmbient = 1;
        mLightingCB->CopyData(static_cast<int>(lightCbIndex), ambientConst);
        mCommandList->SetGraphicsRootConstantBufferView(2, lightingCbAddress(lightCbIndex));
        mCommandList->DrawInstanced(3, 1, 0, 0);
        ++lightCbIndex;

        for (const auto& light : mLights) {
            if (lightCbIndex >= LightingCbElementCount) {
                break;
            }

            LightingConstants lightConst = {};
            lightConst.EnableAmbient = 0;
            lightConst.Light = light;
            mLightingCB->CopyData(static_cast<int>(lightCbIndex), lightConst);
            mCommandList->SetGraphicsRootConstantBufferView(2, lightingCbAddress(lightCbIndex));
            mCommandList->DrawInstanced(3, 1, 0, 0);
            ++lightCbIndex;
        }

        for (const auto& fl : mFallingLights) {
            if (lightCbIndex >= LightingCbElementCount) {
                break;
            }

            LightingConstants lightConst = {};
            lightConst.EnableAmbient = 0;
            lightConst.Light.Type = static_cast<unsigned int>(LightType::Point);
            lightConst.Light.Position = fl.position;
            lightConst.Light.Color = fl.color;
            lightConst.Light.Intensity = fl.intensity;
            lightConst.Light.Range = fl.range;
            mLightingCB->CopyData(static_cast<int>(lightCbIndex), lightConst);
            mCommandList->SetGraphicsRootConstantBufferView(2, lightingCbAddress(lightCbIndex));
            mCommandList->DrawInstanced(3, 1, 0, 0);
            ++lightCbIndex;
        }
    };

    XMFLOAT3 overviewMin(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    XMFLOAT3 overviewMax(
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max());

    for (const auto& object : mSceneObjects) {
        overviewMin.x = (std::min)(overviewMin.x, object.bounds.Center.x - object.bounds.Radius);
        overviewMin.y = (std::min)(overviewMin.y, object.bounds.Center.y - object.bounds.Radius);
        overviewMin.z = (std::min)(overviewMin.z, object.bounds.Center.z - object.bounds.Radius);
        overviewMax.x = (std::max)(overviewMax.x, object.bounds.Center.x + object.bounds.Radius);
        overviewMax.y = (std::max)(overviewMax.y, object.bounds.Center.y + object.bounds.Radius);
        overviewMax.z = (std::max)(overviewMax.z, object.bounds.Center.z + object.bounds.Radius);
    }

    const XMFLOAT3 overviewCenter(
        0.5f * (overviewMin.x + overviewMax.x),
        0.5f * (overviewMin.y + overviewMax.y),
        0.5f * (overviewMin.z + overviewMax.z));
    const float overviewSpan = (std::max)(overviewMax.x - overviewMin.x, overviewMax.z - overviewMin.z) * 1.08f;
    const XMFLOAT3 overviewEye(overviewCenter.x, overviewMax.y + overviewSpan, overviewCenter.z);
    const XMMATRIX overviewView = XMMatrixLookToLH(
        XMLoadFloat3(&overviewEye),
        XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
    const XMMATRIX overviewProj = XMMatrixOrthographicLH(overviewSpan, overviewSpan, 0.1f, overviewSpan * 2.2f);

    const float margin = 18.0f;
    const float overviewSize = std::clamp(static_cast<float>((std::min)(mClientWidth, mClientHeight)) * 0.30f,
                                          180.0f,
                                          300.0f);
    D3D12_VIEWPORT overviewViewport = {};
    overviewViewport.TopLeftX = (std::max)(margin, static_cast<float>(mClientWidth) - overviewSize - margin);
    overviewViewport.TopLeftY = (std::max)(margin, static_cast<float>(mClientHeight) - overviewSize - margin);
    overviewViewport.Width = overviewSize;
    overviewViewport.Height = overviewSize;
    overviewViewport.MinDepth = 0.0f;
    overviewViewport.MaxDepth = 1.0f;

    D3D12_RECT overviewScissor = {
        static_cast<LONG>(overviewViewport.TopLeftX),
        static_cast<LONG>(overviewViewport.TopLeftY),
        static_cast<LONG>(overviewViewport.TopLeftX + overviewViewport.Width),
        static_cast<LONG>(overviewViewport.TopLeftY + overviewViewport.Height)
    };

    writePassConstants(0, view, proj, mEyePos);
    writePassConstants(1, overviewView, overviewProj, overviewEye);

    transitionGBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderGeometryPass(mScreenViewport, mScissorRect, view, proj, 0, 0);
    transitionGBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    auto bbToRt = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &bbToRt);

    renderLightingPass(mScreenViewport, mScissorRect, 0, true);

    transitionGBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderGeometryPass(mScreenViewport, mScissorRect, overviewView, overviewProj, 1, objectCountPerView);
    transitionGBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderLightingPass(overviewViewport, overviewScissor, 1, false);

    auto rtv = CurrentBackBufferView();
    const float borderColor[] = {0.12f, 0.45f, 1.0f, 1.0f};
    constexpr LONG borderThickness = 3;
    D3D12_RECT borderRects[4] = {
        {overviewScissor.left - borderThickness, overviewScissor.top - borderThickness,
         overviewScissor.right + borderThickness, overviewScissor.top},
        {overviewScissor.left - borderThickness, overviewScissor.bottom,
         overviewScissor.right + borderThickness, overviewScissor.bottom + borderThickness},
        {overviewScissor.left - borderThickness, overviewScissor.top,
         overviewScissor.left, overviewScissor.bottom},
        {overviewScissor.right, overviewScissor.top,
         overviewScissor.right + borderThickness, overviewScissor.bottom}
    };
    for (auto& rect : borderRects) {
        rect.left = (std::max)(0L, rect.left);
        rect.top = (std::max)(0L, rect.top);
        rect.right = (std::min)(static_cast<LONG>(mClientWidth), rect.right);
        rect.bottom = (std::min)(static_cast<LONG>(mClientHeight), rect.bottom);
    }
    mCommandList->ClearRenderTargetView(rtv, borderColor, 4, borderRects);

    auto bbToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &bbToPresent);

    ThrowIfFailed(mCommandList->Close(), "Close command list failed");

    ID3D12CommandList* cmdLists[] = {mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1, cmdLists);

    ThrowIfFailed(mSwapChain->Present(1, 0), "Present failed");
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
}

LRESULT DirectXApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_RBUTTONDOWN:
        mLastMousePos.x = GET_X_LPARAM(lParam);
        mLastMousePos.y = GET_Y_LPARAM(lParam);
        SetCapture(mHwnd);
        return 0;

    case WM_RBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (wParam & MK_RBUTTON) {
            const float dx = XMConvertToRadians(0.25f * static_cast<float>(GET_X_LPARAM(lParam) - mLastMousePos.x));
            const float dy = XMConvertToRadians(0.25f * static_cast<float>(GET_Y_LPARAM(lParam) - mLastMousePos.y));

            mYaw += dx;
            mPitch += dy;
            mPitch = std::clamp(mPitch, -1.45f, 1.45f);
        }

        mLastMousePos.x = GET_X_LPARAM(lParam);
        mLastMousePos.y = GET_Y_LPARAM(lParam);
        return 0;

    case WM_SIZE:
        if (mDevice && wParam != SIZE_MINIMIZED) {
            OnResize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

void DirectXApp::FlushCommandQueue() {
    ++mCurrentFence;

    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence), "Signal fence failed");

    if (mFence->GetCompletedValue() < mCurrentFence) {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle), "SetEventOnCompletion failed");
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE DirectXApp::CurrentBackBufferView() const {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
        static_cast<INT>(mCurrBackBuffer),
        mRtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE DirectXApp::DepthStencilView() const {
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

ID3D12Resource* DirectXApp::CurrentBackBuffer() const {
    return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE DirectXApp::GetGpuSrvHandle(unsigned int heapIndex) const {
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(
        mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        static_cast<INT>(heapIndex),
        mCbvSrvUavDescriptorSize);
}
