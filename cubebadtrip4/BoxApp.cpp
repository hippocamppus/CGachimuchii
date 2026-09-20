#include "d3dApp.h"
#include "MathHelper.h"
#include "UploadBuffer.h"
#include <d3dx12.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

using Microsoft::WRL::ComPtr;

using namespace DirectX;
using namespace DirectX::PackedVector;

struct Vertex
{
    XMFLOAT3 Pos;

    XMFLOAT3 Normal;

    XMFLOAT2 TexC;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
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

    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    ComPtr<ID3D12PipelineState> mPSO = nullptr;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;

    float mRadius = 400.0f;

    POINT mLastMousePos;
};

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE prevInstance,
    PSTR cmdLine,
    int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
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

    BuildDescriptorHeaps();
    BuildConstantBuffers();
    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildBoxGeometry();
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

    XMMATRIX world =
        XMLoadFloat4x4(&mWorld);

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

    mObjectCB->CopyData(
        0,
        objConstants
    );
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
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    );

    mCommandList->SetGraphicsRootDescriptorTable(
        0,
        mCbvHeap->GetGPUDescriptorHandleForHeapStart()
    );

    mCommandList->DrawIndexedInstanced(
        mBoxGeo->DrawArgs["box"].IndexCount,
        1,
        0,
        0,
        0
    );

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

void BoxApp::BuildDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;

    cbvHeapDesc.NumDescriptors = 1;
    cbvHeapDesc.Type =
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    cbvHeapDesc.Flags =
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    cbvHeapDesc.NodeMask = 0;

    ThrowIfFailed(
        md3dDevice->CreateDescriptorHeap(
            &cbvHeapDesc,
            IID_PPV_ARGS(&mCbvHeap)
        )
    );
}

void BoxApp::BuildConstantBuffers()
{
    mObjectCB =
        std::make_unique<
        UploadBuffer<ObjectConstants>
        >(
            md3dDevice.Get(),
            1,
            true
        );

    UINT objCBByteSize =
        d3dUtil::CalcConstantBufferByteSize(
            sizeof(ObjectConstants)
        );

    D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
        mObjectCB->Resource()->GetGPUVirtualAddress();

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;

    cbvDesc.BufferLocation =
        cbAddress;

    cbvDesc.SizeInBytes =
        objCBByteSize;

    md3dDevice->CreateConstantBufferView(
        &cbvDesc,
        mCbvHeap->GetCPUDescriptorHandleForHeapStart()
    );
}

void BoxApp::BuildRootSignature()
{
    CD3DX12_ROOT_PARAMETER slotRootParameter[1];

    CD3DX12_DESCRIPTOR_RANGE cbvTable;

    cbvTable.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        1,
        0
    );

    slotRootParameter[0].InitAsDescriptorTable(
        1,
        &cbvTable
    );

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        1,
        slotRootParameter,
        0,
        nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
    );

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;

    HRESULT hr =
        D3D12SerializeRootSignature(
            &rootSigDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(),
            errorBlob.GetAddressOf()
        );

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA(
            static_cast<char*>(
                errorBlob->GetBufferPointer()
                )
        );
    }

    ThrowIfFailed(hr);

    ThrowIfFailed(
        md3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(&mRootSignature)
        )
    );
}

void BoxApp::BuildShadersAndInputLayout()
{
    mvsByteCode =
        d3dUtil::CompileShader(
            L"Shaders\\color.hlsl",
            nullptr,
            "VS",
            "vs_5_0"
        );

    mpsByteCode =
        d3dUtil::CompileShader(
            L"Shaders\\color.hlsl",
            nullptr,
            "PS",
            "ps_5_0"
        );

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
                reader.Error().c_str()
            );
        }

        throw std::runtime_error(
            "Failed to load OBJ"
        );
    }

    const tinyobj::attrib_t& attrib =
        reader.GetAttrib();

    const std::vector<tinyobj::shape_t>& shapes =
        reader.GetShapes();

    DirectX::XMFLOAT3 bmin(
        +FLT_MAX,
        +FLT_MAX,
        +FLT_MAX
    );

    DirectX::XMFLOAT3 bmax(
        -FLT_MAX,
        -FLT_MAX,
        -FLT_MAX
    );

    for (
        size_t i = 0;
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
            (std::min)(
                bmin.x,
                x
                );

        bmin.y =
            (std::min)(
                bmin.y,
                y
                );

        bmin.z =
            (std::min)(
                bmin.z,
                z
                );

        bmax.x =
            (std::max)(
                bmax.x,
                x
                );

        bmax.y =
            (std::max)(
                bmax.y,
                y
                );

        bmax.z =
            (std::max)(
                bmax.z,
                z
                );
    }

    XMFLOAT3 center(
        0.5f * (bmin.x + bmax.x),
        0.5f * (bmin.y + bmax.y),
        0.5f * (bmin.z + bmax.z)
    );

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    for (const auto& shape : shapes)
    {
        for (const auto& idx : shape.mesh.indices)
        {
            if (idx.vertex_index < 0)
            {
                continue;
            }

            Vertex v;

            const int vi =
                3 * idx.vertex_index;

            float x =
                attrib.vertices[vi + 0];

            float y =
                attrib.vertices[vi + 1];

            float z =
                attrib.vertices[vi + 2];

            v.Pos =
                DirectX::XMFLOAT3(
                    x - center.x,
                    y - center.y,
                    z - center.z
                );

            v.Normal =
                XMFLOAT3(
                    0.0f,
                    1.0f,
                    0.0f
                );

            if (idx.normal_index >= 0)
            {
                const size_t ni =
                    static_cast<size_t>(
                        3 * idx.normal_index
                        );

                if (ni + 2 < attrib.normals.size())
                {
                    v.Normal =
                        XMFLOAT3(
                            attrib.normals[ni + 0],
                            attrib.normals[ni + 1],
                            attrib.normals[ni + 2]
                        );
                }
            }

            v.TexC =
                XMFLOAT2(
                    0.0f,
                    0.0f
                );

            if (idx.texcoord_index >= 0)
            {
                const size_t ti =
                    static_cast<size_t>(
                        2 * idx.texcoord_index
                        );

                if (ti + 1 < attrib.texcoords.size())
                {
                    v.TexC =
                        XMFLOAT2(
                            attrib.texcoords[ti + 0],
                            1.0f -
                            attrib.texcoords[ti + 1]
                        );
                }
            }

            vertices.push_back(v);

            indices.push_back(
                static_cast<std::uint32_t>(
                    indices.size()
                    )
            );
        }
    }

    const UINT vbByteSize =
        static_cast<UINT>(
            vertices.size() *
            sizeof(Vertex)
            );

    const UINT ibByteSize =
        static_cast<UINT>(
            indices.size() *
            sizeof(std::uint32_t)
            );

    mBoxGeo =
        std::make_unique<MeshGeometry>();

    mBoxGeo->Name =
        "Sponza";

    ThrowIfFailed(
        D3DCreateBlob(
            vbByteSize,
            &mBoxGeo->VertexBufferCPU
        )
    );

    CopyMemory(
        mBoxGeo->VertexBufferCPU->GetBufferPointer(),
        vertices.data(),
        vbByteSize
    );

    ThrowIfFailed(
        D3DCreateBlob(
            ibByteSize,
            &mBoxGeo->IndexBufferCPU
        )
    );

    CopyMemory(
        mBoxGeo->IndexBufferCPU->GetBufferPointer(),
        indices.data(),
        ibByteSize
    );

    mBoxGeo->VertexBufferGPU =
        d3dUtil::CreateDefaultBuffer(
            md3dDevice.Get(),
            mCommandList.Get(),
            vertices.data(),
            vbByteSize,
            mBoxGeo->VertexBufferUploader
        );

    mBoxGeo->IndexBufferGPU =
        d3dUtil::CreateDefaultBuffer(
            md3dDevice.Get(),
            mCommandList.Get(),
            indices.data(),
            ibByteSize,
            mBoxGeo->IndexBufferUploader
        );

    mBoxGeo->VertexByteStride =
        sizeof(Vertex);

    mBoxGeo->VertexBufferByteSize =
        vbByteSize;

    mBoxGeo->IndexFormat =
        DXGI_FORMAT_R32_UINT;

    mBoxGeo->IndexBufferByteSize =
        ibByteSize;

    SubmeshGeometry submesh;

    submesh.IndexCount =
        static_cast<UINT>(
            indices.size()
            );

    submesh.StartIndexLocation =
        0;

    submesh.BaseVertexLocation =
        0;

    mBoxGeo->DrawArgs["box"] =
        submesh;
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

    psoDesc.PS =
    {
        reinterpret_cast<BYTE*>(
            mpsByteCode->GetBufferPointer()
        ),
        mpsByteCode->GetBufferSize()
    };

    psoDesc.RasterizerState =
        CD3DX12_RASTERIZER_DESC(
            D3D12_DEFAULT
        );

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
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

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