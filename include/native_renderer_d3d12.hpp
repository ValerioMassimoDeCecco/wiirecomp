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

    // Risorse per visualizzazione del framebuffer nativo Wii (XFB / YUYV422)
    ID3D12Resource* xfbTexture = nullptr;
    ID3D12Resource* xfbUpload = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;

    UINT frameIndex = 0;
    HANDLE fenceEvent = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;

    bool CreateXfbPipeline() {
        // 1. Root Signature con 1 SRV (t0), 1 Root Constant (b0, 4 float) e 1 Static Sampler (s0)
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParams[2] = {};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[0].DescriptorTable.pDescriptorRanges = &range;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[1].Constants.ShaderRegister = 0;
        rootParams[1].Constants.RegisterSpace = 0;
        rootParams[1].Constants.Num32BitValues = 4;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = rootParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* signatureBlob = nullptr;
        ID3DBlob* errorBlob = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob))) {
            if (errorBlob) {
                std::cerr << "[D3D12] Errore serializzazione Root Signature: " << (char*)errorBlob->GetBufferPointer() << "\n";
                errorBlob->Release();
            }
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)))) {
            signatureBlob->Release();
            return false;
        }
        signatureBlob->Release();

        // 2. Compilazione Shaders HLSL per Fullscreen Quad
        const char* xfbShaderCode = R"(
            cbuffer XfbScale : register(b0) {
                float2 u_UvScale;
                float2 u_Pad;
            };
            struct PS_INPUT {
                float4 pos : SV_POSITION;
                float2 uv  : TEXCOORD0;
            };
            PS_INPUT VSMain(uint vid : SV_VertexID) {
                PS_INPUT o;
                o.uv = float2((vid << 1) & 2, vid & 2);
                o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
                return o;
            }
            Texture2D    g_Texture : register(t0);
            SamplerState g_Sampler : register(s0);
            float4 PSMain(PS_INPUT i) : SV_Target {
                return g_Texture.Sample(g_Sampler, i.uv * u_UvScale);
            }
        )";

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        HRESULT hr = D3DCompile(xfbShaderCode, strlen(xfbShaderCode), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) { std::cerr << "[D3D12] VS Compile error: " << (char*)errorBlob->GetBufferPointer() << "\n"; errorBlob->Release(); }
            return false;
        }
        hr = D3DCompile(xfbShaderCode, strlen(xfbShaderCode), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) { std::cerr << "[D3D12] PS Compile error: " << (char*)errorBlob->GetBufferPointer() << "\n"; errorBlob->Release(); }
            vsBlob->Release();
            return false;
        }

        // 3. Pipeline State Object (PSO)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = rootSignature;
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
        vsBlob->Release();
        psBlob->Release();
        if (FAILED(hr)) return false;

        // 4. Descriptor Heap per SRV (Shader-Visible)
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 1;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)))) return false;

        // 5. Creazione XFB 2D Texture (fino a 720x576 RGBA8 per NTSC/PAL)
        static constexpr uint32_t MAX_XFB_TEX_W = 720;
        static constexpr uint32_t MAX_XFB_TEX_H = 576;
        static constexpr uint32_t MAX_XFB_ROW_PITCH = ((MAX_XFB_TEX_W * 4 + 255) & ~255u); // 3072

        D3D12_HEAP_PROPERTIES defaultHeapProps = {};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = MAX_XFB_TEX_W;
        texDesc.Height = MAX_XFB_TEX_H;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(device->CreateCommittedResource(
            &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
            &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&xfbTexture)))) return false;

        // SRV per la texture
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(xfbTexture, &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());

        // 6. Creazione Upload Buffer per la texture
        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = (UINT64)MAX_XFB_ROW_PITCH * MAX_XFB_TEX_H;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
            &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
            &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&xfbUpload)))) return false;

        return true;
    }

    bool Initialize(HWND windowHandle, uint32_t w, uint32_t h) {
        hwnd = windowHandle;
        width = w;
        height = h;

        // 1. Creazione Factory DXGI & Device D3D12
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) {
            std::cout << "[D3D12] Hardware device non disponibile, fallback su software WARP adapter...\n";
            IDXGIAdapter* warpAdapter = nullptr;
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)))) {
                hr = D3D12CreateDevice(warpAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
                warpAdapter->Release();
            }
        }
        if (FAILED(hr) || !device) {
            std::cerr << "[D3D12] Creazione D3D12 device fallita (HRESULT: 0x" << std::hex << hr << std::dec << ")\n";
            return false;
        }

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

        // 7. Pipeline grafica per XFB (External Framebuffer / libogc video)
        if (!CreateXfbPipeline()) {
            std::cout << "[D3D12] Avviso: pipeline XFB texture non inizializzata, rendering solo clear.\n";
            Log("[D3D12] Avviso: pipeline XFB texture non inizializzata, rendering solo clear.");
        }

        std::string d3dMsg = "[D3D12] Pipeline DirectX 12 inizializzata con successo (" + std::to_string(width) + "x" + std::to_string(height) + ")";
        std::cout << d3dMsg << "\n";
        Log(d3dMsg);
        return true;
    }

    // Renderizza il framebuffer nativo Wii (XFB) con VSync.
    void RenderFrame(const std::vector<UniversalVertex>& vertices, const uint8_t* xfb = nullptr,
                     uint32_t fbWidth = 640, uint32_t fbHeight = 480) {
        (void)vertices;
        if (!device || !swapChain || !commandAllocator || !commandList) return;

        if (fbWidth < 16 || fbWidth > 720) fbWidth = 640;
        if (fbHeight < 16 || fbHeight > 576) fbHeight = 480;
        static constexpr uint32_t MAX_XFB_TEX_W = 720;
        static constexpr uint32_t MAX_XFB_TEX_H = 576;
        static constexpr uint32_t MAX_XFB_ROW_PITCH = ((MAX_XFB_TEX_W * 4 + 255) & ~255u);

        commandAllocator->Reset();
        commandList->Reset(commandAllocator, pipelineState);

        bool xfbUploaded = false;
        if (xfb != nullptr && xfbUpload && xfbTexture) {
            uint8_t* pUpload = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            if (SUCCEEDED(xfbUpload->Map(0, &readRange, reinterpret_cast<void**>(&pUpload)))) {
                const uint8_t* pSrc = xfb;
                const uint32_t dstStrideWords = MAX_XFB_ROW_PITCH / 4;
                uint32_t* pDstBase = reinterpret_cast<uint32_t*>(pUpload);

                for (uint32_t y = 0; y < fbHeight; ++y) {
                    uint32_t* pDst = pDstBase + y * dstStrideWords;
                    for (uint32_t x = 0; x + 1 < fbWidth; x += 2) {
                        uint8_t y0 = pSrc[0];
                        uint8_t u  = pSrc[1];
                        uint8_t y1 = pSrc[2];
                        uint8_t v  = pSrc[3];
                        pSrc += 4;

                        if ((y0 | u | y1 | v) == 0) {
                            pDst[x]     = 0xFF000000u;
                            pDst[x + 1] = 0xFF000000u;
                            continue;
                        }

                        int c0 = (int)y0 - 16; if (c0 < 0) c0 = 0;
                        int c1 = (int)y1 - 16; if (c1 < 0) c1 = 0;
                        int d = (int)u - 128;
                        int e = (int)v - 128;

                        auto ToRgba = [](int c, int d, int e) -> uint32_t {
                            int r = (298 * c + 409 * e + 128) >> 8;
                            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                            int b = (298 * c + 516 * d + 128) >> 8;
                            if (r < 0) r = 0; else if (r > 255) r = 255;
                            if (g < 0) g = 0; else if (g > 255) g = 255;
                            if (b < 0) b = 0; else if (b > 255) b = 255;
                            return (0xFF000000u) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
                        };

                        pDst[x]     = ToRgba(c0, d, e);
                        pDst[x + 1] = ToRgba(c1, d, e);
                    }
                }
                xfbUpload->Unmap(0, nullptr);

                D3D12_RESOURCE_BARRIER bCopy = {};
                bCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bCopy.Transition.pResource = xfbTexture;
                bCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                bCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &bCopy);

                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                dstLoc.pResource = xfbTexture;
                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dstLoc.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                srcLoc.pResource = xfbUpload;
                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcLoc.PlacedFootprint.Offset = 0;
                srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srcLoc.PlacedFootprint.Footprint.Width = MAX_XFB_TEX_W;
                srcLoc.PlacedFootprint.Footprint.Height = MAX_XFB_TEX_H;
                srcLoc.PlacedFootprint.Footprint.Depth = 1;
                srcLoc.PlacedFootprint.Footprint.RowPitch = MAX_XFB_ROW_PITCH;

                D3D12_BOX srcBox = { 0, 0, 0, fbWidth, fbHeight, 1 };
                commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);

                bCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                commandList->ResourceBarrier(1, &bCopy);
                xfbUploaded = true;
            }
        }

        // Imposta la transizione da Present a Render Target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = renderTargets[frameIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);

        UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
        rtvHandle.ptr += frameIndex * rtvDescriptorSize;

        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        const float clearColor[] = { 0.05f, 0.05f, 0.05f, 1.0f };
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        if (xfbUploaded && srvHeap && pipelineState && rootSignature) {
            commandList->SetGraphicsRootSignature(rootSignature);
            ID3D12DescriptorHeap* ppHeaps[] = { srvHeap };
            commandList->SetDescriptorHeaps(1, ppHeaps);
            commandList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
            float uvConsts[4] = { (float)fbWidth / (float)MAX_XFB_TEX_W, (float)fbHeight / (float)MAX_XFB_TEX_H, 0.0f, 0.0f };
            commandList->SetGraphicsRoot32BitConstants(1, 4, uvConsts, 0);

            D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
            D3D12_RECT sr = { 0, 0, (LONG)width, (LONG)height };
            commandList->RSSetViewports(1, &vp);
            commandList->RSSetScissorRects(1, &sr);

            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawInstanced(3, 1, 0, 0);
        }

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
