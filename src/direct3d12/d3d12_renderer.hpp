#ifndef D3D12_RENDERER_HPP_
#define D3D12_RENDERER_HPP_

#ifdef WINDOW_PLATFORM_WIN32

#include "d3d12_model.hpp"
#include "d3d12_skybox.hpp"
#include "renderer.hpp"
#include "utils.hpp"
#include "win32/win32_window.hpp"

#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

class Scene;

class D3D12Renderer : public Renderer
{
public:
    D3D12Renderer(Window& window,
                  const DrawProperties& drawProps,
                  const Camera& camera);
    ~D3D12Renderer() override;
    DISABLE_COPY_AND_MOVE(D3D12Renderer)

    bool init() final;
    void initImGuiBackend() final;
    void cleanup() final;
    void draw(const Scene& scene) final;
    void setVSyncEnabled(const bool vsyncEnabled) final;

private:
    // See this link for HLSL Constant Buffer layout rules:
    // https://maraneshi.github.io/HLSL-ConstantBufferLayoutVisualizer/
    static constexpr size_t CONSTANT_BUFFER_ALIGNMENT = 256;
#define CB_ALIGNMENT_ASSERT(cb, name)                            \
    static_assert((sizeof(cb) % CONSTANT_BUFFER_ALIGNMENT) == 0, \
                  name " size must be 256-byte aligned")
    struct alignas(CONSTANT_BUFFER_ALIGNMENT) MVPConstantBuffer
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMFLOAT4X4 normalMatrix;
    };
    CB_ALIGNMENT_ASSERT(MVPConstantBuffer, "MVP Constant Buffer");

    struct alignas(CONSTANT_BUFFER_ALIGNMENT) MaterialConstantBuffer
    {
        DirectX::XMFLOAT3 color;
        float specularReflectivity;
        DirectX::XMFLOAT3 viewPos;
        float shininess;
        DirectX::XMFLOAT3 lightDirection;
        uint8_t padding[212];
    };
    CB_ALIGNMENT_ASSERT(MaterialConstantBuffer, "Material Constant Buffer");

    static constexpr auto FRAME_COUNT = 2;
    static constexpr DXGI_FORMAT DEPTH_STENCIL_FORMAT
        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    static constexpr auto MAX_MODEL_SCENE_NODE_COUNT = 256;

    CD3DX12_VIEWPORT viewport_;
    CD3DX12_RECT scissorRect_;

    winrt::com_ptr<ID3D12Device> device_;
    winrt::com_ptr<IDXGISwapChain3> swapChain_;
    winrt::com_ptr<ID3D12CommandQueue> commandQueue_;
    winrt::com_ptr<ID3D12CommandAllocator> commandAllocator_;
    winrt::com_ptr<ID3D12GraphicsCommandList> commandList_;
    winrt::com_ptr<ID3D12DescriptorHeap> rtvDescriptorHeap_;
    UINT rtvDescriptorHeapSize_;
    winrt::com_ptr<ID3D12DescriptorHeap> dsvDescriptorHeap_;

    static constexpr auto NULL_CBV_COUNT = 1;
    static constexpr auto CB_PER_MODEL_COUNT = 2;
    static constexpr auto SKYBOX_TEXTURE_SRV_COUNT = 1;
    static constexpr auto IMGUI_FONT_TEXTURE_SRV_COUNT = 1;
    static constexpr auto MAX_CBV_SRV_UAV_DESCRIPTOR_COUNT
        = NULL_CBV_COUNT + (MAX_MODEL_SCENE_NODE_COUNT * CB_PER_MODEL_COUNT)
        + SKYBOX_TEXTURE_SRV_COUNT + IMGUI_FONT_TEXTURE_SRV_COUNT;
    winrt::com_ptr<ID3D12DescriptorHeap> cbvSrvUavDescriptorHeap_;
    UINT cbvSrvUavDescriptorHeapSize_;

    std::array<winrt::com_ptr<ID3D12Resource>, FRAME_COUNT> renderTargets_;
    winrt::com_ptr<ID3D12Resource> depthStencil_;
    // TODO: Move model PSO out of D3D12Renderer
    winrt::com_ptr<ID3D12RootSignature> modelRootSignature_;

    enum class ModelPSOInstance : uint8_t
    {
        Gouraud,
        Phong,
        BlinnPhong,

        Count,
    };
    std::array<winrt::com_ptr<ID3D12PipelineState>,
               static_cast<size_t>(ModelPSOInstance::Count)>
        modelPsos_;

    std::vector<winrt::com_ptr<ID3D12Resource>> mvpConstantBuffers_;
    std::vector<winrt::com_ptr<ID3D12Resource>> materialConstantBuffers_;
    std::vector<uint8_t*> mvpCbvDataBegin_;
    std::vector<uint8_t*> materialCbvDataBegin_;

    winrt::com_ptr<ID3D12Fence> fence_;
    UINT frameIndex_;
    UINT64 fenceValue_;
    HANDLE fenceEvent_;

    std::vector<D3D12Model> models_;
    D3D12Skybox skybox_;

    bool tearingEnabled_;
    bool vsyncEnabled_;

    bool createDevice();
    bool createCommandObjects();
    bool createDescriptorHeaps();
    bool createRTVs();
    bool createDSV();
    bool createCBVs();
    template <typename ConstantBufferType>
    bool createConstantBufferAndView(
        const CD3DX12_HEAP_PROPERTIES& heapProperties,
        winrt::com_ptr<ID3D12Resource>& constantBuffer,
        INT cbvDescriptorHeapIndex,
        uint8_t*& cbvDataBegin);
    bool createModelRootSignature();
    bool createModelPSO();
    bool createSyncObjects();
    bool loadAssets();

    void drawModels(const Scene& scene,
                    MVPConstantBuffer& mvpConstantBufferData,
                    MaterialConstantBuffer& materialConstantBufferData);
    void drawGui();
    void screenshot();
    void waitForPreviousFrame();
};

inline void D3D12Renderer::setVSyncEnabled(const bool vsyncEnabled)
{
    vsyncEnabled_ = vsyncEnabled;
}

#endif  // WINDOW_PLATFORM_WIN32
#endif  // D3D12_RENDERER_HPP_
