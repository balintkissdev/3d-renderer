#include "d3d12_renderer.hpp"

#include "camera.hpp"
#include "d3d12_shader.hpp"
#include "drawproperties.hpp"
#include "globals.hpp"
#include "scene.hpp"
#include "utils.hpp"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "stb_image_write.h"

#include <combaseapi.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <synchapi.h>

#ifdef PIX_ENABLED
#include "WinPixEventRuntime/pix3.h"
#endif

namespace fs = std::filesystem;
using namespace DirectX;
using namespace winrt;

namespace
{
D3D12_GPU_DESCRIPTOR_HANDLE s_nullCbvGpuHandle;
}  // namespace

D3D12Renderer::D3D12Renderer(Window& window,
                             const DrawProperties& drawProps,
                             const Camera& camera)
    : Renderer{window, drawProps, camera}
    , viewport_{CD3DX12_VIEWPORT(
          0.0f,
          0.0f,
          static_cast<float>(window_.frameBufferSize().first),
          static_cast<float>(window_.frameBufferSize().second))}
    , scissorRect_{0,
                   0,
                   window_.frameBufferSize().first,
                   window_.frameBufferSize().second}
    , rtvDescriptorHeapSize_{0}
    , cbvSrvUavDescriptorHeapSize_{0}
    , currentFrameIndex_{0}
    , fenceValues_{}
    , tearingEnabled_{false}
    , vsyncEnabled_{drawProps.vsyncEnabled}
{
    models_.reserve(3);
}

D3D12Renderer::~D3D12Renderer()
{
    cleanup();
}

bool D3D12Renderer::init()
{
    if (!createDevice() || !createDescriptorHeaps() || !createFrameResources()
        || !createDSV() || !createCBVs() || !createModelRootSignature()
        || !createModelPSO() || !loadAssets())
    {
        return false;
    }

    createSyncObjects();
    // Wait for setup to complete
    waitForGPU();

    setVSyncEnabled(drawProps_.vsyncEnabled);
    return true;
}

void D3D12Renderer::cleanup()
{
    waitForGPU();  // Avoid corruption of resources
    ::CloseHandle(fenceEvent_);

#if defined(DEBUG) || defined(_DEBUG)
    // If debug layer is kept being enabled when switching from OpenGL back to
    // Direct3D 12 backend, results in crash when debug layer is being enabled
    // twice on the same adapter. Alternative is guarding first time debug layer
    // enabling with a static bool to do lazy initialization.
    com_ptr<ID3D12Debug> debugController;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(debugController.put()));
    if (SUCCEEDED(hr))
    {
        com_ptr<ID3D12Debug4> debugController4;
        hr = debugController->QueryInterface(debugController4.put());
        if (SUCCEEDED(hr))
        {
            debugController4->DisableDebugLayer();
        }
    }
#endif
}

// TODO: Add code to setup WARP device for compatibility
bool D3D12Renderer::createDevice()
{
    HRESULT hr;
    UINT deviceFlags = 0;

#if defined(DEBUG) || defined(_DEBUG)
    {
        com_ptr<ID3D12Debug> debugController;
        hr = D3D12GetDebugInterface(IID_PPV_ARGS(debugController.put()));
        if (SUCCEEDED(hr))
        {
            debugController->EnableDebugLayer();
            deviceFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    com_ptr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory2(deviceFlags, IID_PPV_ARGS(factory.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to create DXGI factory");
        return false;
    }

    // Get adapter
    com_ptr<IDXGIAdapter1> adapter;
    com_ptr<IDXGIFactory6> factory6;
    hr = factory->QueryInterface(factory6.put());
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to create DXGI factory");
        return false;
    }

    hr = factory6->EnumAdapterByGpuPreference(
        0,
        DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
        IID_PPV_ARGS(adapter.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to query GPU adapter");
        return false;
    }

    DXGI_ADAPTER_DESC1 adapterDesc;
    adapter->GetDesc1(&adapterDesc);

    hr = D3D12CreateDevice(adapter.get(),
                           D3D_FEATURE_LEVEL_11_1,
                           IID_PPV_ARGS(device_.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to initialize Direct3D 12 device");
        return false;
    }

    // Create Command Queue (Swap Chain creation requires it)
    D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
    commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&commandQueueDesc,
                                     IID_PPV_ARGS(commandQueue_.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to create Command Queue");
        return false;
    }

    // Query tearing support for uncapped framerate with VSync off and DXGI flip
    // model enabled
    BOOL allowTearing = false;  // Careful because the Win32 BOOL sizeof (int)
                                // is not the same as C++ bool size and would
                                // result in invalid call, it is what it is
    hr = factory6->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                       &allowTearing,
                                       sizeof(allowTearing));
    assert(hr != DXGI_ERROR_INVALID_CALL
           && "invalid call for tearing support query detected");
    tearingEnabled_ = SUCCEEDED(hr) && allowTearing;

    // Create Swap Chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = MAX_FRAME_COUNT;
    swapChainDesc.Width = window_.frameBufferSize().first;
    swapChainDesc.Height = window_.frameBufferSize().second;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // According to
    // https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect,
    // never use blit model DXGI_SWAP_EFFECT_DISCARD (implicit 0) because it is
    // never supported for Direct3D 12 apps. Use the DXGI flip model instead.
    //
    // Handling of unthrottled framerate becomes different though after
    // transitioning from blit to flip model, as now you have to manually query
    // support for allowing tearing. More info on the DXGI flip model available
    // here:
    // https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
    // https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags
        = tearingEnabled_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;

    com_ptr<IDXGISwapChain1> swapChain;
    // From Direct3D 11.1 and onwards, Microsoft recommends using
    // CreateSwapChainForHwnd instead of CreateSwapChain
    hr = factory->CreateSwapChainForHwnd(commandQueue_.get(),
                                         window_.raw(),
                                         &swapChainDesc,
                                         nullptr,
                                         nullptr,
                                         swapChain.put());
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to create Swap Chain");
        return false;
    }
    swapChain_ = swapChain.try_as<IDXGISwapChain3>();
    if (!swapChain_)
    {
        utils::showErrorMessage("unable to create Swap Chain");
        return false;
    }
    // Fullscreen transitions not supported
    factory->MakeWindowAssociation(window_.raw(), DXGI_MWA_NO_ALT_ENTER);
    currentFrameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    return true;
}

bool D3D12Renderer::createFrameResources()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle(
        rtvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());

    for (size_t frame = 0; frame < commandAllocators_.size(); ++frame)
    {
        // Render Target View (RTV) for each frame
        swapChain_->GetBuffer(frame, IID_PPV_ARGS(renderTargets_[frame].put()));
        device_->CreateRenderTargetView(renderTargets_[frame].get(),
                                        nullptr,
                                        rtvCpuHandle);
        rtvCpuHandle.Offset(1, rtvDescriptorHeapSize_);

        // Command Allocator for each frame
        HRESULT hr = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(commandAllocators_[frame].put()));
        if (FAILED(hr))
        {
            utils::showErrorMessage(
                "unable to create Command Allocator for frame",
                frame);
            return false;
        }
    }

    return true;
}

bool D3D12Renderer::createDescriptorHeaps()
{
    // Render Target View (RTV) descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = MAX_FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(
            &rtvHeapDesc,
            IID_PPV_ARGS(rtvDescriptorHeap_.put()))))
    {
        utils::showErrorMessage(
            "unable to create Render Target View (RTV) descriptor heap");
        return false;
    }
    rtvDescriptorHeapSize_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Depth-Stencil View (DSV) descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(
            &dsvHeapDesc,
            IID_PPV_ARGS(dsvDescriptorHeap_.put()))))
    {
        utils::showErrorMessage(
            "unable to create Depth-Stencil View (DSV) descriptor heap");
        return false;
    }

    // CBV/SRV/UAV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc{};
    cbvSrvUavHeapDesc.NumDescriptors = MAX_CBV_SRV_UAV_DESCRIPTOR_COUNT;
    cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(
            &cbvSrvUavHeapDesc,
            IID_PPV_ARGS(cbvSrvUavDescriptorHeap_.put()))))
    {
        utils::showErrorMessage("unable to create CBV/SRV/UAV descriptor heap");
        return false;
    }
    cbvSrvUavDescriptorHeapSize_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

bool D3D12Renderer::createDSV()
{
    const CD3DX12_HEAP_PROPERTIES depthStencilHeapProps(
        D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC depthStencilTextureDesc
        = CD3DX12_RESOURCE_DESC::Tex2D(DEPTH_STENCIL_FORMAT,
                                       window_.frameBufferSize().first,
                                       window_.frameBufferSize().second,
                                       1,
                                       0,
                                       1,
                                       0,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_CLEAR_VALUE depthOptimizedClearValue = {
        .Format = DEPTH_STENCIL_FORMAT,
        .DepthStencil = {
            .Depth = 1.0f,
            .Stencil = 0,
        },
    };
    if (FAILED(device_->CreateCommittedResource(
            &depthStencilHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &depthStencilTextureDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthOptimizedClearValue,
            IID_PPV_ARGS(depthStencil_.put()))))
    {
        utils::showErrorMessage("unable to create Depth-Stencil texture");
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc{};
    depthStencilViewDesc.Format = DEPTH_STENCIL_FORMAT;
    depthStencilViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depthStencilViewDesc.Flags = D3D12_DSV_FLAG_NONE;
    device_->CreateDepthStencilView(
        depthStencil_.get(),
        &depthStencilViewDesc,
        dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());

    return true;
}

// TODO: Refactor duplicate code
bool D3D12Renderer::createCBVs()
{
    // Null resource CBV (for debugging)
    INT cbvIndexOffset = 0;
    CD3DX12_CPU_DESCRIPTOR_HANDLE nullCbvCpuHandle(
        cbvSrvUavDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
        cbvIndexOffset,
        cbvSrvUavDescriptorHeapSize_);
    D3D12_CONSTANT_BUFFER_VIEW_DESC nullCbvDesc{};
    device_->CreateConstantBufferView(&nullCbvDesc, nullCbvCpuHandle);
    s_nullCbvGpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        cbvSrvUavDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
        cbvIndexOffset,
        cbvSrvUavDescriptorHeapSize_);
    ++cbvIndexOffset;

    mvpConstantBuffers_.resize(MAX_MODEL_SCENE_NODE_COUNT);
    mvpCbvDataBegin_.resize(MAX_MODEL_SCENE_NODE_COUNT);
    materialConstantBuffers_.resize(MAX_MODEL_SCENE_NODE_COUNT);
    materialCbvDataBegin_.resize(MAX_MODEL_SCENE_NODE_COUNT);
    const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    for (int i = 0; i < MAX_MODEL_SCENE_NODE_COUNT; ++i)
    {
        // Create MVP constant buffer and view
        if (!createConstantBufferAndView<MVPConstantBuffer>(
                heapProperties,
                mvpConstantBuffers_[i],
                cbvIndexOffset + i,
                mvpCbvDataBegin_[i]))
        {
            return false;
        }

        // Create Material constant buffer and view
        if (!createConstantBufferAndView<MaterialConstantBuffer>(
                heapProperties,
                materialConstantBuffers_[i],
                cbvIndexOffset + MAX_MODEL_SCENE_NODE_COUNT + i,
                materialCbvDataBegin_[i]))
        {
            return false;
        }
    }

    return true;
}

template <typename ConstantBufferType>
bool D3D12Renderer::createConstantBufferAndView(
    const CD3DX12_HEAP_PROPERTIES& heapProperties,
    winrt::com_ptr<ID3D12Resource>& constantBuffer,
    INT cbvDescriptorHeapIndex,
    uint8_t*& cbvDataBegin)
{
    // Create Constant Buffer
    constexpr UINT constantBufferSize = sizeof(ConstantBufferType);
    const CD3DX12_RESOURCE_DESC resourceDesc
        = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
    HRESULT hr
        = device_->CreateCommittedResource(&heapProperties,
                                           D3D12_HEAP_FLAG_NONE,
                                           &resourceDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr,
                                           IID_PPV_ARGS(constantBuffer.put()));
    if (FAILED(hr))
    {
        return false;
    }

    // Create Constant Buffer View (CBV)
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = constantBufferSize;
    CD3DX12_CPU_DESCRIPTOR_HANDLE cbvCpuHandle(
        cbvSrvUavDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
        cbvDescriptorHeapIndex,
        cbvSrvUavDescriptorHeapSize_);
    device_->CreateConstantBufferView(&cbvDesc, cbvCpuHandle);

    // Map and initialize Constant Buffer
    CD3DX12_RANGE readRange(
        0,
        0);  // We do not intend to read from this resource on the CPU
    constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&cbvDataBegin));

    return true;
}

bool D3D12Renderer::createModelRootSignature()
{
    // Root Signature acts as "function signature" for the shaders in the
    // pipeline, defining the parameters passed for the shaders.
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData{};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE,
                                            &featureData,
                                            sizeof(featureData))))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    // With descriptor table of CVBs
    std::array<CD3DX12_DESCRIPTOR_RANGE1, 2> ranges;
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                   1,
                   0,
                   0,
                   D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                   1,
                   1,
                   0,
                   D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
    // Parameters passed to the shaders are called Root Parameters
    std::array<CD3DX12_ROOT_PARAMETER1, 2> rootParameters;
    rootParameters[0].InitAsDescriptorTable(1,
                                            ranges.data(),
                                            D3D12_SHADER_VISIBILITY_VERTEX);
    rootParameters[1].InitAsDescriptorTable(
        1,
        &ranges[1],
        D3D12_SHADER_VISIBILITY_ALL);  // TODO: Optimize

    constexpr D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags
        = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(static_cast<UINT>(rootParameters.size()),
                               rootParameters.data(),
                               0,
                               nullptr,
                               rootSignatureFlags);
    com_ptr<ID3DBlob> signature;
    com_ptr<ID3DBlob> error;
    D3DX12SerializeVersionedRootSignature(&rootSignatureDesc,
                                          D3D_ROOT_SIGNATURE_VERSION_1,
                                          signature.put(),
                                          error.put());
    HRESULT hr
        = device_->CreateRootSignature(0,
                                       signature->GetBufferPointer(),
                                       signature->GetBufferSize(),
                                       IID_PPV_ARGS(modelRootSignature_.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to create Root Signature");
        return false;
    }
    return true;
}

bool D3D12Renderer::createModelPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};

    // Root Signature
    psoDesc.pRootSignature = modelRootSignature_.get();

    // Input Layout
    struct VertexSpecification
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
    };
    const std::array<D3D12_INPUT_ELEMENT_DESC, 2> inputElementDesc = {
        D3D12_INPUT_ELEMENT_DESC{"POSITION",
                                 0,
                                 DXGI_FORMAT_R32G32B32_FLOAT,
                                 0,
                                 offsetof(VertexSpecification, position),
                                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                                 0},
        D3D12_INPUT_ELEMENT_DESC{"NORMAL",
                                 0,
                                 DXGI_FORMAT_R32G32B32_FLOAT,
                                 0,
                                 offsetof(VertexSpecification, normal),
                                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                                 0},
    };
    psoDesc.InputLayout
        = D3D12_INPUT_LAYOUT_DESC{inputElementDesc.data(),
                                  static_cast<UINT>(inputElementDesc.size())};

    // Topology type
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Rasterizer State
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // Show backfaces (e.g. faces inside Utah Teapot)
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Depth-Stencil State
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // Blend State
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = renderTargets_[0]->GetDesc().Format;
    psoDesc.DSVFormat = depthStencil_->GetDesc().Format;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    // Shaders
    const std::array<D3D12Shader::Params,
                     static_cast<size_t>(ModelPSOInstance::Count)>
        shaderPermutations{
            {{.shaderBaseName = "gouraud", .defines = {}},
             {.shaderBaseName = "phong", .defines = {}},
             {.shaderBaseName = "phong", .defines = {"BLINN_PHONG"}}}};
    for (size_t i = 0; i < shaderPermutations.size(); ++i)
    {
        // TODO: Revisit HLSL file organization when using precompiled CSO
        // bytecode
        com_ptr<ID3DBlob> vertexShader;
        if (!D3D12Shader::Compile(shaderPermutations[i],
                                  D3D12Shader::ShaderCompileType::VertexShader,
                                  vertexShader))
        {
            return false;
        }
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.get());

        com_ptr<ID3DBlob> pixelShader;
        if (!D3D12Shader::Compile(shaderPermutations[i],
                                  D3D12Shader::ShaderCompileType::PixelShader,
                                  pixelShader))
        {
            return false;
        }
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.get());

        HRESULT hr = device_->CreateGraphicsPipelineState(
            &psoDesc,
            IID_PPV_ARGS(modelPsos_[i].put()));
        if (FAILED(hr))
        {
            utils::showErrorMessage(
                "unable to create Pipeline State Object (PSO)");
            return false;
        }
    }
    return true;
}

bool D3D12Renderer::createSyncObjects()
{
    // Direct3D 12 GPU hardware fence
    HRESULT hr = device_->CreateFence(0,
                                      D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(fence_.put()));
    if (FAILED(hr))
    {
        HRESULT removedReason = E_FAIL;
        if (device_)
        {
            removedReason = device_->GetDeviceRemovedReason();
        }
        utils::showErrorMessage("unable to create fence", hr, removedReason);
        return false;
    }

    fenceValues_[currentFrameIndex_] = 1;

    // Win32 OS Event handle to make thread scheduler sleep or wake up the CPU
    fenceEvent_ = ::CreateEvent(nullptr, false, false, nullptr);
    if (!fenceEvent_)
    {
        const DWORD e = ::GetLastError();
        utils::showErrorMessage("unable to create fence event", e);
        return false;
    }

    return true;
}

bool D3D12Renderer::loadAssets()
{
    // Command list
    HRESULT hr = device_->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocators_[currentFrameIndex_].get(),
        nullptr,
        IID_PPV_ARGS(commandList_.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to create Command List");
        return false;
    }
    // By default, command lists are created in "Record" state and have to be
    // set to "Closed" state later. Keep it open for now to record skybox
    // texture (and other resource) upload commands.

    // Models
    const std::array<fs::path, 3> modelPaths{"assets/meshes/cube.obj",
                                             "assets/meshes/teapot.obj",
                                             "assets/meshes/bunny.obj"};
    for (const auto& path : modelPaths)
    {
        std::optional<D3D12Model> model
            = D3D12Model::create(device_.get(), path);
        if (!model)
        {
            utils::showErrorMessage("unable to create model from path ", path);
            return false;
        }
        models_.emplace_back(std::move(model.value()));
    }

    // Skybox
    constexpr UINT skyboxSrvDescriptorOffset
        = MAX_MODEL_SCENE_NODE_COUNT * CB_PER_MODEL_COUNT;
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(
        cbvSrvUavDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
        skyboxSrvDescriptorOffset,
        cbvSrvUavDescriptorHeapSize_);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(
        cbvSrvUavDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
        skyboxSrvDescriptorOffset,
        cbvSrvUavDescriptorHeapSize_);

    std::optional<D3D12Skybox> skybox
        = D3D12SkyboxBuilder().build(device_.get(),
                                     commandList_.get(),
                                     srvCpuHandle,
                                     srvGpuHandle);

    if (!skybox)
    {
        utils::showErrorMessage("unable to create skybox for application");
        return false;
    }
    skybox_ = std::move(skybox.value());

    // Flush command list to apply texture uploads
    commandList_->Close();
    ID3D12CommandList* commandLists[] = {commandList_.get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);

    return true;
}

void D3D12Renderer::initImGuiBackend()
{
    ImGui_ImplWin32_Init(window_.raw());

    const UINT imguiSrvDescriptorOffset
        = MAX_CBV_SRV_UAV_DESCRIPTOR_COUNT - IMGUI_FONT_TEXTURE_SRV_COUNT;
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(
        cbvSrvUavDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
        imguiSrvDescriptorOffset,
        cbvSrvUavDescriptorHeapSize_);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(
        cbvSrvUavDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
        imguiSrvDescriptorOffset,
        cbvSrvUavDescriptorHeapSize_);

    // TODO: This is the legacy way of initializing a Direct3D12 ImGui backend.
    // From version 1.91.6, all ImGui backend versions require specifying a
    // separate ImGui_ImplDX12_InitInfo structure and the use of SRV descriptor
    // heap allocators.
    //
    // See:
    // https://github.com/ocornut/imgui/blob/c0dfd65d6790b9b96872b64fa232f1fa80fcd3b3/examples/example_win32_directx12/main.cpp#L37
    ImGui_ImplDX12_Init(device_.get(),
                        MAX_FRAME_COUNT,
                        renderTargets_[0]->GetDesc().Format,
                        cbvSrvUavDescriptorHeap_.get(),
                        srvCpuHandle,
                        srvGpuHandle);
}

void D3D12Renderer::draw(const Scene& scene)
{
    // TODO: Update transformation calculations to reflect calculations in
    // GLRenderer
    MVPConstantBuffer mvpConstantBufferData;
    XMMATRIX projection
        = XMMatrixPerspectiveFovRH(XMConvertToRadians(drawProps_.fieldOfView),
                                   ASPECT_RATIO,
                                   NEAR_CLIP_DISTANCE_Z,
                                   FAR_CLIP_DISTANCE_Z);
    XMStoreFloat4x4(&mvpConstantBufferData.projection,
                    XMMatrixTranspose(projection));

    // TODO: Conversion between GLM and DXMath matrices is a performance
    // overhead
    const glm::mat4 glmView = camera_.calculateViewMatrix();
    // clang-format off
    XMMATRIX dxView = XMMatrixSet(
        glmView[0][0], glmView[1][0], glmView[2][0], glmView[3][0],
        glmView[0][1], glmView[1][1], glmView[2][1], glmView[3][1],
        glmView[0][2], glmView[1][2], glmView[2][2], glmView[3][2],
        glmView[0][3], glmView[1][3], glmView[2][3], glmView[3][3]
    );
    // clang-format on
    XMStoreFloat4x4(&mvpConstantBufferData.view, dxView);

    MaterialConstantBuffer materialConstantBufferData;
    materialConstantBufferData.viewPos.x = camera_.position().x;
    materialConstantBufferData.viewPos.y = camera_.position().y;
    materialConstantBufferData.viewPos.z = camera_.position().z;
    materialConstantBufferData.lightDirection.x = drawProps_.lightDirection[0];
    materialConstantBufferData.lightDirection.y = drawProps_.lightDirection[1];
    materialConstantBufferData.lightDirection.z = drawProps_.lightDirection[2];

    // Begin frame
    commandAllocators_[currentFrameIndex_]->Reset();
    commandList_->Reset(commandAllocators_[currentFrameIndex_].get(), nullptr);
    PIX_EVENT(commandList_.get(), "D3D12Renderer::draw");

    // Set render state
    commandList_->SetGraphicsRootSignature(modelRootSignature_.get());
    commandList_->SetPipelineState(
        modelPsos_[static_cast<size_t>(drawProps_.lightingModel)].get());

    // CBV/SRV/UAV heaps
    const std::array<ID3D12DescriptorHeap*, 1> cbvSrvUavHeaps
        = {cbvSrvUavDescriptorHeap_.get()};
    commandList_->SetDescriptorHeaps(static_cast<UINT>(cbvSrvUavHeaps.size()),
                                     cbvSrvUavHeaps.data());

    // Set Rasterizer Stage state
    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);

    // Set backbuffer as render target
    ID3D12Resource* renderTarget = renderTargets_[currentFrameIndex_].get();
    CD3DX12_RESOURCE_BARRIER transitionBarrier
        = CD3DX12_RESOURCE_BARRIER::Transition(
            renderTarget,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &transitionBarrier);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        rtvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(),
        static_cast<int>(currentFrameIndex_),
        rtvDescriptorHeapSize_);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(
        dsvDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());
    commandList_->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

    // Record drawing commands
    const std::array<float, 4> clearColor = {drawProps_.backgroundColor[0],
                                             drawProps_.backgroundColor[1],
                                             drawProps_.backgroundColor[2],
                                             1.0f};
    commandList_->ClearRenderTargetView(rtvHandle,
                                        clearColor.data(),
                                        0,
                                        nullptr);
    commandList_->ClearDepthStencilView(dsvHandle,
                                        D3D12_CLEAR_FLAG_DEPTH,
                                        1.0f,
                                        0,
                                        0,
                                        nullptr);

    drawModels(scene, mvpConstantBufferData, materialConstantBufferData);
    if (drawProps_.skyboxEnabled)
    {
        // HACK: Don't pass model matrix to skybox shader
        skybox_.draw(commandList_.get(), mvpConstantBuffers_[0].get());
    }
    drawGui();

    // End frame
    transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        renderTarget,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    commandList_->ResourceBarrier(1, &transitionBarrier);
    commandList_->Close();

    const std::array<ID3D12CommandList*, 1> commandLists = {commandList_.get()};
    commandQueue_->ExecuteCommandLists(static_cast<UINT>(commandLists.size()),
                                       commandLists.data());

    if (Globals::takingScreenshot)
    {
        screenshot();
        Globals::takingScreenshot = false;
    }

    // TODO: When time comes to support fullscreen, make sure to update present
    // flags according to
    // https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Fullscreen/src/D3D12Fullscreen.cpp
    //
    // Presenting with tearing allowed while VSync is on is illegal and you will
    // go to rendering jail.
    const UINT presentFlags
        = (!vsyncEnabled_ && tearingEnabled_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swapChain_->Present(vsyncEnabled_, presentFlags);
    assert(SUCCEEDED(hr) && "invalid swapchain present call detected");
    waitForPreviousFrame();
}

void D3D12Renderer::drawModels(
    const Scene& scene,
    MVPConstantBuffer& mvpConstantBufferData,
    MaterialConstantBuffer& materialConstantBufferData)
{
    PIX_EVENT(commandList_.get(), "D3D12Renderer::drawModels");

    for (size_t i = 0; i < scene.children().size(); ++i)
    {
        const SceneNode& sceneNode = scene.children()[i];
        const D3D12Model* model = &models_[sceneNode.modelID];

        // Model transform
        // Translate
        XMMATRIX modelMatrix = XMMatrixTranslation(sceneNode.position.x,
                                                   sceneNode.position.y,
                                                   sceneNode.position.z);

        // Avoid Gimbal-lock by converting Euler angles to quaternions
        const XMVECTOR quatX = XMQuaternionRotationAxis(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
            XMConvertToRadians(sceneNode.rotation.x));
        const XMVECTOR quatY = XMQuaternionRotationAxis(
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
            XMConvertToRadians(sceneNode.rotation.y));
        const XMVECTOR quatZ = XMQuaternionRotationAxis(
            XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
            XMConvertToRadians(sceneNode.rotation.z));
        const XMVECTOR quat
            = XMQuaternionMultiply(XMQuaternionMultiply(quatX, quatY), quatZ);
        const XMMATRIX rotation = XMMatrixRotationQuaternion(quat);

        modelMatrix = XMMatrixMultiply(rotation, modelMatrix);
        XMStoreFloat4x4(&mvpConstantBufferData.model,
                        XMMatrixTranspose(modelMatrix));

        // No need to transpose, as the inverse matrix is already in the correct
        // transposed state
        XMMATRIX normalMatrix = XMMatrixInverse(nullptr, modelMatrix);
        XMStoreFloat4x4(&mvpConstantBufferData.normalMatrix, normalMatrix);

        // MVP constant buffer
        std::memcpy(mvpCbvDataBegin_[i],
                    &mvpConstantBufferData,
                    sizeof(mvpConstantBufferData));
        CD3DX12_GPU_DESCRIPTOR_HANDLE mvpCbvGpuHandle(
            cbvSrvUavDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
            NULL_CBV_COUNT + static_cast<int>(i),
            cbvSrvUavDescriptorHeapSize_);
        commandList_->SetGraphicsRootDescriptorTable(0, mvpCbvGpuHandle);

        // Material constant buffer
        materialConstantBufferData.color.x = sceneNode.color.r;
        materialConstantBufferData.color.y = sceneNode.color.g;
        materialConstantBufferData.color.z = sceneNode.color.b;
        materialConstantBufferData.specularReflectivity
            = sceneNode.specularReflectivity;
        materialConstantBufferData.shininess = sceneNode.shininess;

        std::memcpy(materialCbvDataBegin_[i],
                    &materialConstantBufferData,
                    sizeof(materialConstantBufferData));
        CD3DX12_GPU_DESCRIPTOR_HANDLE materialCbvGpuHandle(
            cbvSrvUavDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
            NULL_CBV_COUNT + static_cast<int>(mvpConstantBuffers_.size() + i),
            cbvSrvUavDescriptorHeapSize_);
        commandList_->SetGraphicsRootDescriptorTable(1, materialCbvGpuHandle);

        // Issue draw call
        // TODO: Wireframe fill mode requires creation of a separate PSO with
        // rasterizer fill mode set to D3D12_FILL_MODE_WIREFRAME. Do not use
        // D3D_PRIMITIVE_TOPOLOGY_LINELIST, because it will have missing edges.
        commandList_->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList_->IASetVertexBuffers(0, 1, &model->vertexBuffer());
        commandList_->IASetIndexBuffer(&model->indexBuffer());
        commandList_->DrawIndexedInstanced(model->indexCount(), 1, 0, 0, 0);
    }
}

void D3D12Renderer::drawGui()
{
    PIX_EVENT(commandList_.get(), "D3D12Renderer::drawGui");
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList_.get());
}

void D3D12Renderer::screenshot()
{
    // Wait for drawing to finish
    waitForGPU();

    commandAllocators_[currentFrameIndex_]->Reset();
    commandList_->Reset(commandAllocators_[currentFrameIndex_].get(), nullptr);

    // Backbuffer (GPU)
    com_ptr<ID3D12Resource> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(currentFrameIndex_,
                                       IID_PPV_ARGS(backBuffer.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage("unable to query backbuffer for screenshot");
        return;
    }
    D3D12_RESOURCE_DESC backBufferDesc = backBuffer->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT rowCount;
    UINT64 rowByteSize;
    UINT64 totalByteCount;
    device_->GetCopyableFootprints(&backBufferDesc,
                                   0,
                                   1,
                                   0,
                                   &footprint,
                                   &rowCount,
                                   &rowByteSize,
                                   &totalByteCount);

    // Readback buffer (readback Heap, CPU, also known as Staging Texture)
    const CD3DX12_HEAP_PROPERTIES readbackHeapProps(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC readbackBufferDesc
        = CD3DX12_RESOURCE_DESC::Buffer(totalByteCount);
    com_ptr<ID3D12Resource> readbackBuffer;
    hr = device_->CreateCommittedResource(&readbackHeapProps,
                                          D3D12_HEAP_FLAG_NONE,
                                          &readbackBufferDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr,
                                          IID_PPV_ARGS(readbackBuffer.put()));
    if (FAILED(hr))
    {
        utils::showErrorMessage(
            "unable to allocate readback buffer for screenshot");
        return;
    }

    // Copy texture
    CD3DX12_RESOURCE_BARRIER transitionBarrier
        = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList_->ResourceBarrier(1, &transitionBarrier);

    const CD3DX12_TEXTURE_COPY_LOCATION src(backBuffer.get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION dst(readbackBuffer.get(), footprint);
    commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    transitionBarrier
        = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.get(),
                                               D3D12_RESOURCE_STATE_COPY_SOURCE,
                                               D3D12_RESOURCE_STATE_PRESENT);
    commandList_->ResourceBarrier(1, &transitionBarrier);

    commandList_->Close();
    ID3D12CommandList* commandLists[] = {commandList_.get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);

    // Wait until copy is finished
    waitForGPU();

    // Read data into CPU buffer
    void* readbackBufferData = nullptr;
    D3D12_RANGE readRange{0, totalByteCount};
    hr = readbackBuffer->Map(0, &readRange, &readbackBufferData);
    if (FAILED(hr))
    {
        utils::showErrorMessage(
            "unable to map screenshot readback buffer into memory");
        return;
    }
    DEFER({
        D3D12_RANGE writeRange(0, 0);
        readbackBuffer->Unmap(0, &writeRange);
    });

    // TODO: PNG output is not optimal, it is 20-50% larger than a file
    // written by a decent optimizing implementation
    constexpr const char* screenshotFilename = "screenshot.png";
    constexpr int rgbaChannelCount = 4;
    // Psst, switching to OpenGL backend changes flip, so switch back to default
    // orientation
    stbi_flip_vertically_on_write(false);
    const int fileWriteSuccess
        = stbi_write_png(screenshotFilename,
                         // back buffer and reaadback buffer are not supposed to
                         // have the same dimensions
                         static_cast<int>(backBufferDesc.Width),
                         static_cast<int>(backBufferDesc.Height),
                         rgbaChannelCount,
                         readbackBufferData,
                         static_cast<int>(footprint.Footprint.RowPitch));
    if (fileWriteSuccess == 0)
    {
        utils::showErrorMessage("unable to write screenshot into file",
                                screenshotFilename);
        return;
    }
}

void D3D12Renderer::waitForGPU()
{
    // Ask GPU to write fence value when commands that were submitted before
    // "signal" command are done.
    commandQueue_->Signal(fence_.get(), fenceValues_[currentFrameIndex_]);

    // Skip unnecessary CPU wait if GPU is already caught up with finished work.
    // GPU is faster then you think, it could have even finished processing the
    // command queue and reached the "signal" command right before CPU stepped
    // to the next code line in here.
    if (fence_->GetCompletedValue() < fenceValues_[currentFrameIndex_])
    {
        // Ask GPU fence to trigger Win32 Event when done
        fence_->SetEventOnCompletion(fenceValues_[currentFrameIndex_],
                                     fenceEvent_);

        // Sleep CPU until fence has been processed. This is not some
        // power-wasting spinlock, Windows thread scheduler suspends this thread
        // and yields control to other apps or OS tasks.
        ::WaitForSingleObject(fenceEvent_, INFINITE);
    }

    // Fence value is incremented for next synchronization point in current
    // frame, not for next frame itself.
    ++fenceValues_[currentFrameIndex_];
}

void D3D12Renderer::waitForPreviousFrame()
{
    // Schedule "signal" command for CURRENT frame
    const UINT64 currentFenceValue = fenceValues_[currentFrameIndex_];
    commandQueue_->Signal(fence_.get(), currentFenceValue);

    // Step to next frame
    currentFrameIndex_ = swapChain_->GetCurrentBackBufferIndex();

    if (fence_->GetCompletedValue() < fenceValues_[currentFrameIndex_])
    {
        fence_->SetEventOnCompletion(fenceValues_[currentFrameIndex_],
                                     fenceEvent_);
        ::WaitForSingleObject(fenceEvent_, INFINITE);
    }

    // Fence value for next frame
    fenceValues_[currentFrameIndex_] = currentFenceValue + 1;
}
