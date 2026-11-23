#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <vector>
#include <iostream>
#include "gx_fifo.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace WiiPort {

// Shader HLSL che replica la combinazione TEV (Texture Environment) della GPU Hollywood
// Equazione TEV: Result = clamp(D + ((1 - C) * A + C * B) * Scale)
static const char* g_TevShaderHLSL = R"(
struct VS_INPUT {
    float3 pos   : POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

struct PS_INPUT {
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

cbuffer ScreenConstants : register(b0) {
    float4x4 u_Transform;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    output.pos   = mul(u_Transform, float4(input.pos, 1.0f));
    output.color = input.color;
    output.uv    = input.uv;
    return output;
}

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 PSMain(PS_INPUT input) : SV_Target {
    float4 texColor = g_Texture.Sample(g_Sampler, input.uv);
    // TEV Stage 0: Modulazione tra colore vertice e texture
    float3 A = texColor.rgb;
    float3 B = input.color.rgb;
    float3 C = float3(1.0f, 1.0f, 1.0f);
    float3 D = float3(0.0f, 0.0f, 0.0f);

    float3 finalColor = saturate(D + ((1.0f - C) * A + C * B * texColor.rgb));
    float finalAlpha = texColor.a * input.color.a;

    return float4(finalColor, finalAlpha);
}
)";

class D3D12Renderer {
public:
    HWND hwnd = nullptr;
    uint32_t width = 1280;
    uint32_t height = 720;

    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12Resource* renderTargets[2] = { nullptr, nullptr };
    ID3D12CommandAllocator* commandAllocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState* pipelineState = nullptr;

    ID3D12Resource* vertexBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;

    UINT frameIndex = 0;
    HANDLE fenceEvent = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;

    bool Initialize(HWND windowHandle, uint32_t w, uint32_t h) {
        hwnd = windowHandle;
        width = w;
        height = h;

        // 1. Creazione Factory DXGI & Device D3D12
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;

        // 2. Creazione Command Queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)))) return false;

        // 3. Creazione Swap Chain a doppio buffer
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.BufferCount = 2;
        scDesc.Width = width;
        scDesc.Height = height;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.SampleDesc.Count = 1;

        IDXGISwapChain1* tempSwapChain = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(commandQueue, hwnd, &scDesc, nullptr, nullptr, &tempSwapChain))) {
            return false;
        }
        swapChain = static_cast<IDXGISwapChain3*>(tempSwapChain);
        frameIndex = swapChain->GetCurrentBackBufferIndex();

        // 4. Creazione RTV Heap
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 2;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) return false;

        UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());

        for (UINT n = 0; n < 2; n++) {
            if (FAILED(swapChain->GetBuffer(n, IID_PPV_ARGS(&renderTargets[n])))) return false;
            device->CreateRenderTargetView(renderTargets[n], nullptr, rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
        }

        // 5. Command Allocator & List
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)))) return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr, IID_PPV_ARGS(&commandList)))) return false;
        commandList->Close();

        // 6. Fence per la sincronizzazione CPU-GPU
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        std::cout << "[D3D12] Pipeline DirectX 12 inizializzata con successo (" << width << "x" << height << ")\n";
        return true;
    }

    // Renderizza la geometria estratta dal GX Command Processor e sincronizza con VSync
    void RenderFrame(const std::vector<UniversalVertex>& vertices) {
        if (vertices.empty()) return;

        commandAllocator->Reset();
        commandList->Reset(commandAllocator, pipelineState);

        // Imposta la transizione da Present a Render Target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = renderTargets[frameIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);

        UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
        rtvHandle.ptr += frameIndex * rtvDescriptorSize;

        const float clearColor[] = { 0.1f, 0.1f, 0.15f, 1.0f };
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        // Transizione di ritorno a Present
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &barrier);

        commandList->Close();

        ID3D12CommandList* ppCommandLists[] = { commandList };
        commandQueue->ExecuteCommandLists(1, ppCommandLists);

        // Swap Buffer con VSync abilitato (1)
        swapChain->Present(1, 0);

        // Avanza indice e sincronizza fence
        const UINT64 currentFenceValue = fenceValue;
        commandQueue->Signal(fence, currentFenceValue);
        fenceValue++;

        if (fence->GetCompletedValue() < currentFenceValue) {
            fence->SetEventOnCompletion(currentFenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        frameIndex = swapChain->GetCurrentBackBufferIndex();
    }
};

} // namespace WiiPort
