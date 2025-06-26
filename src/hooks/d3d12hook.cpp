#include "d3d12hook.h"
#include <kiero/kiero.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <iostream>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <thread>

// Debug
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")

typedef HRESULT (__stdcall *PresentFunc)(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags);

PresentFunc oPresent = nullptr;

typedef void (__stdcall *ExecuteCommandListsFunc)(ID3D12CommandQueue *pCommandQueue, UINT NumCommandLists,
                                                  ID3D12CommandList *const *ppCommandLists);

ExecuteCommandListsFunc oExecuteCommandLists = nullptr;

typedef HRESULT (__stdcall *ResizeBuffers)(IDXGISwapChain3 *pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                           DXGI_FORMAT NewFormat, UINT SwapChainFlags);

ResizeBuffers oResizeBuffers;

typedef HRESULT (__stdcall *SignalFunc)(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value);

SignalFunc oSignal = nullptr;

HWND window;
WNDPROC oWndProc;

struct FrameContext {
    ID3D12CommandAllocator *CommandAllocator;
    UINT64 FenceValue; // In imgui original code // i didn't use it
    ID3D12Resource *g_mainRenderTargetResource = {};
    D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor = {};
};

// Data
static int const NUM_FRAMES_IN_FLIGHT = 3;
// static FrameContext*                g_frameContext[NUM_FRAMES_IN_FLIGHT] = {};
//  Modified
FrameContext *g_frameContext;
static UINT g_frameIndex = 0;
static UINT g_fenceValue = 0;

// static int const                    NUM_BACK_BUFFERS = 3; // original
static int NUM_BACK_BUFFERS = -1;
static ID3D12Device *g_pd3dDevice = nullptr;
static ID3D12DescriptorHeap *g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap *g_pd3dSrvDescHeap = nullptr;
static ID3D12CommandQueue *g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList *g_pd3dCommandList = nullptr;
static ID3D12Fence *g_fence = nullptr;
static HANDLE g_fenceEvent = nullptr;
static UINT64 g_fenceLastSignaledValue = 0;
static IDXGISwapChain3 *g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static HANDLE g_hSwapChainWaitableObject = nullptr;
// static ID3D12Resource*              g_mainRenderTargetResource[NUM_BACK_BUFFERS] = {}; // Original
// static D3D12_CPU_DESCRIPTOR_HANDLE  g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS] = {}; // Original
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

bool bMenu = false;

void CreateRenderTarget() {
    // Проверяем все необходимые указатели
    if (!g_pSwapChain || !g_pd3dDevice || !g_frameContext || NUM_BACK_BUFFERS <= 0) {
        LOG_ERROR("Cannot create render target - DirectX objects not initialized");
        return;
    }

    for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
        ID3D12Resource *pBackBuffer = nullptr;
        if (FAILED(g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer)))) {
            LOG_ERROR("Failed to get back buffer");
            return;
        }
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_frameContext[i].g_mainRenderTargetDescriptor);
        g_frameContext[i].g_mainRenderTargetResource = pBackBuffer;
    }
}

void CleanupRenderTarget() {
    if (g_frameContext && NUM_BACK_BUFFERS > 0) {
        for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
            if (g_frameContext[i].g_mainRenderTargetResource) {
                g_frameContext[i].g_mainRenderTargetResource->Release();
                g_frameContext[i].g_mainRenderTargetResource = nullptr;
            }
        }
    }
}

void WaitForLastSubmittedFrame() {
    FrameContext *frameCtx = &g_frameContext[g_frameIndex % NUM_FRAMES_IN_FLIGHT];

    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue == 0)
        return; // No fence was signaled

    frameCtx->FenceValue = 0;
    if (g_fence->GetCompletedValue() >= fenceValue)
        return;

    g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT APIENTRY WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
        return true;

    return CallWindowProc(oWndProc, hwnd, uMsg, wParam, lParam);
}

void InitImGui() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void) io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls

    // Setup Dear ImGui style
    // ImGui::StyleColorsDark();
    ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX12_Init(g_pd3dDevice, NUM_FRAMES_IN_FLIGHT,
                        DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
                        g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                        g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());
    ImGui_ImplDX12_CreateDeviceObjects();
}

HRESULT __fastcall hkPresent(IDXGISwapChain3 *pSwapChain, UINT SyncInterval, UINT Flags) {
    static bool init = false;

    if (!init) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void **)&g_pd3dDevice))) {
            // Get swap chain description
            DXGI_SWAP_CHAIN_DESC sdesc;
            pSwapChain->GetDesc(&sdesc);
            sdesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            sdesc.Windowed = ((GetWindowLongPtr(window, GWL_STYLE) & WS_POPUP) != 0) ? false : true;
            window = sdesc.OutputWindow;

            // sDesc1
            DXGI_SWAP_CHAIN_DESC1 sdesc1;
            pSwapChain->GetDesc1(&sdesc1);

            NUM_BACK_BUFFERS = sdesc.BufferCount;
            g_frameContext = new FrameContext[NUM_BACK_BUFFERS];

            // RTV Descriptor Heap

            {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                desc.NumDescriptors = NUM_BACK_BUFFERS;
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                desc.NodeMask = 1;
                if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK)
                    return E_FAIL;

                SIZE_T rtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

                // Create RenderTargetView

                for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
                    g_frameContext[i].g_mainRenderTargetDescriptor = rtvHandle;
                    rtvHandle.ptr += rtvDescriptorSize;
                }
            }

            // SRV Descriptor Heap
            {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                desc.NumDescriptors = 1;
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK)
                    return E_FAIL;
            } {
                ID3D12CommandAllocator *allocator;
                if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) !=
                    S_OK)
                    return E_FAIL;

                for (size_t i = 0; i < NUM_BACK_BUFFERS; i++) {
                    g_frameContext[i].CommandAllocator = allocator;
                }

                if (g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                                                    IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
                    g_pd3dCommandList->Close() != S_OK)
                    return E_FAIL;
            }

            g_fenceEvent = CreateEvent(nullptr, false, false, nullptr);
            if (g_fenceEvent == nullptr)
                assert(g_fenceEvent == nullptr);

            g_hSwapChainWaitableObject = pSwapChain->GetFrameLatencyWaitableObject();
            g_pSwapChain = pSwapChain;

            CreateRenderTarget();
            oWndProc = (WNDPROC) SetWindowLongPtr(window, GWLP_WNDPROC, (__int3264) (LONG_PTR) WndProc);
            InitImGui();

            // Add fullscreen mode support
            DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
            pSwapChain->GetDesc1(&swapChainDesc);

            // Enable Alt+Enter and resize support
            DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
            fullscreenDesc.Windowed = TRUE;
            fullscreenDesc.RefreshRate.Numerator = 60;
            fullscreenDesc.RefreshRate.Denominator = 1;
            fullscreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
            fullscreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        }
        init = true;
    }

    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
        WaitForLastSubmittedFrame();
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_ResizeWidth = g_ResizeHeight = 0;
        CreateRenderTarget();
    }

    if (!g_pd3dCommandQueue) {
        printf("Failed to create command queue\n");
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    ImGuiStyle *style = &ImGui::GetStyle();


    ImGui::StyleColorsClassic();
    style->WindowPadding = ImVec2(8, 8);
    style->WindowRounding = 5.0f;
    style->FramePadding = ImVec2(4, 2);
    style->FrameRounding = 0.0f;
    style->ItemSpacing = ImVec2(8, 4);
    style->ItemInnerSpacing = ImVec2(4, 4);
    style->IndentSpacing = 21.0f;
    style->ScrollbarSize = 14.0f;
    style->ScrollbarRounding = 0.0f;
    style->GrabMinSize = 10.0f;
    style->GrabRounding = 0.0f;
    style->TabRounding = 0.f;
    style->ChildRounding = 0.0f;
    style->WindowBorderSize = 1.f;
    style->ChildBorderSize = 1.f;
    style->PopupBorderSize = 0.f;
    style->FrameBorderSize = 0.f;
    style->TabBorderSize = 0.f;

    style->Colors[ImGuiCol_Text] = ImVec4(0.000f, 0.678f, 0.929f, 1.0f);
    style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.0f, 0.0263f, 0.0357f, 1.00f);
    style->Colors[ImGuiCol_WindowBg] = ImVec4(0.059f, 0.051f, 0.071f, 1.00f);
    style->Colors[ImGuiCol_ChildBg] = ImVec4(0.071f, 0.071f, 0.090f, 1.00f);
    style->Colors[ImGuiCol_PopupBg] = ImVec4(0.0f, 0.0263f, 0.0357f, 1.00f);
    style->Colors[ImGuiCol_Border] = ImColor(0.000f, 0.678f, 0.929f, 1.0f);
    style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0263f, 0.0357f, 0.00f);
    style->Colors[ImGuiCol_FrameBg] = ImVec4(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_TitleBg] = ImVec4(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.263f, 0.357f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
    style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
    style->Colors[ImGuiCol_CheckMark] = ImColor(87, 119, 134, 255);
    style->Colors[ImGuiCol_SliderGrab] = ImColor(119, 134, 169, 150);
    style->Colors[ImGuiCol_SliderGrabActive] = ImColor(119, 134, 169, 150);
    style->Colors[ImGuiCol_Button] = ImColor(26, 23, 31, 255);
    style->Colors[ImGuiCol_ButtonHovered] = ImColor(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_ButtonActive] = ImColor(0.102f, 0.090f, 0.122f, 1.000f);
    style->Colors[ImGuiCol_Header] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_HeaderActive] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_Separator] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_SeparatorActive] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style->Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style->Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style->Colors[ImGuiCol_PlotLines] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_PlotHistogram] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);
    style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(1.00f, 0.98f, 0.95f, 0.73f);

    style->WindowTitleAlign.x = 0.50f;
    style->FrameRounding = 0.0f;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    static int MenuTab = 0;
    float
            TextSpaceLine = 90.f,
            SpaceLineOne = 120.f,
            SpaceLineTwo = 280.f,
            SpaceLineThr = 420.f;
    static const char *HitboxList[]{"Head", "Neck", "Chest", "Pelvis"};
    static int SelectedHitbox = 0;

    static const char *MouseKeys[]{"RMouse", "LMouse", "Control", "Shift", "Alt"};
    static int KeySelected = 0;

    if (GetAsyncKeyState(VK_INSERT) & 1) bMenu = !bMenu;
    if (bMenu) {
        ImGui::GetForegroundDrawList()->AddCircleFilled(ImGui::GetIO().MousePos, float(4), ImColor(255, 0, 0), 50);

        ImGui::SetNextWindowSize({620.f, 350.f});

        ImGui::Begin("rensa", 0,
                     ImGuiWindowFlags_::ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar);
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        ImGui::SetCursorPos({36.f, 31.f});
        ImGui::Text("by fishptr");
        ImGui::SetCursorPos({22.f, 56.f});
        if (ImGui::Button("Aimbot", {89.f, 32.f})) {
            MenuTab = 0;
        }
        ImGui::SetCursorPos({22.f, 93.f});
        if (ImGui::Button("Visuals", {89.f, 32.f})) {
            MenuTab = 1;
        }
        ImGui::SetCursorPos({22.f, 130.f});
        if (ImGui::Button("MISC", {89.f, 32.f})) {
            MenuTab = 2;
        }
        /*
        ImGui::SetCursorPos({22.f, 204.f});
        if (ImGui::Button("Discord", {89.f, 32.f})) {
            system("start https://discord.gg/8jTAstg4GK");
        }
        ImGui::SetCursorPos({22.f, 291.f});
        if (ImGui::Button("unload", {65.f, 20.f})) {
            exit(0);
        }
        */
        style->ItemSpacing = ImVec2(8, 8);

        if (MenuTab == 0) {
            ImGui::SetCursorPos({137.f, 39.f});
            ImGui::BeginChild("##Aimbot", {450.f, 279.f}, true);
            ImGui::SetCursorPos({19.f, 14.f});
            ImGui::Text("Aim:");
            //ImGui::Checkbox("Aimbot", &bAimbot);
            //ImGui::SliderFloat("Smooth", &Smooth, 2, 15);
            //ImGui::SliderInt("Fov Size", &FovSize, 50, 600);
        }
        if (MenuTab == 1) {
            ImGui::SetCursorPos({137.f, 39.f});
            ImGui::BeginChild("##Visuals", {450.f, 279.f}, true);
            ImGui::SetCursorPos({19.f, 14.f});
            ImGui::Text("Enemy:");
            //ImGui::Checkbox("Corner Box", &bCornerBox);
        }
        if (MenuTab == 2) {
            ImGui::SetCursorPos({137.f, 39.f});
            ImGui::BeginChild("##Misc", {450.f, 279.f}, true);
            ImGui::SetCursorPos({19.f, 14.f});
            ImGui::Text("Gay:");
        }
        ImGui::EndChild();
        ImGui::End();
    }

    FrameContext &frameCtx = g_frameContext[pSwapChain->GetCurrentBackBufferIndex()];
    frameCtx.CommandAllocator->Reset();

    UINT backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_frameContext[backBufferIdx].g_mainRenderTargetResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    g_pd3dCommandList->Reset(frameCtx.CommandAllocator, nullptr);
    g_pd3dCommandList->ResourceBarrier(1, &barrier);

    g_pd3dCommandList->OMSetRenderTargets(1, &g_frameContext[backBufferIdx].g_mainRenderTargetDescriptor, FALSE,
                                          nullptr);
    g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    g_pd3dCommandList->ResourceBarrier(1, &barrier);
    g_pd3dCommandList->Close();

    g_pd3dCommandQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&g_pd3dCommandList));

    return oPresent(pSwapChain, SyncInterval, Flags);
}

void __fastcall hkExecuteCommandLists(ID3D12CommandQueue *pCommandQueue, UINT NumCommandLists,
                                      ID3D12CommandList *const *ppCommandLists) {
    if (!g_pd3dCommandQueue) {
        g_pd3dCommandQueue = pCommandQueue;
    }

    oExecuteCommandLists(pCommandQueue, NumCommandLists, ppCommandLists);
}

HRESULT __fastcall hkResizeBuffers(IDXGISwapChain3 *pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                   DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // Проверяем готовность к изменению размера
    if (!g_pd3dDevice || !g_pSwapChain) {
        LOG_ERROR("Cannot resize - DirectX objects not initialized");
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (g_pd3dDevice) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
    }

    CleanupRenderTarget();

    // Сохраняем новое количество буферов
    NUM_BACK_BUFFERS = BufferCount;

    // Вызываем оригинальную функцию
    HRESULT result = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(result)) {
        // Пересоздаем наши ресурсы только если успешно изменили размер
        CreateRenderTarget();
        if (g_pd3dDevice) {
            ImGui_ImplDX12_CreateDeviceObjects();
        }
    } else {
        LOG_ERROR("ResizeBuffers failed with error code: 0x%X", result);
    }

    return result;
}

HRESULT __fastcall hkSignal(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value) {
    if (g_pd3dCommandQueue != nullptr && queue == g_pd3dCommandQueue) {
        g_fence = fence;
        g_fenceValue = value;
    }
    return oSignal(queue, fence, value);;
}

bool InitD3D12Hook() {
    LOG_INFO("Waiting for process initialization...");

    HANDLE d3d12Module = nullptr;
    HANDLE dxgiModule = nullptr;

    while (true) {
        d3d12Module = GetModuleHandleA("d3d12.dll");
        dxgiModule = GetModuleHandleA("dxgi.dll");

        if (d3d12Module && dxgiModule)
            break;

        if (WaitForSingleObject(GetCurrentProcess(), 1000) != WAIT_TIMEOUT) {
            LOG_ERROR("Process terminated while waiting for DirectX");
            return false;
        }

        LOG_INFO("Waiting for DirectX modules...");
    }

    LOG_INFO("DirectX modules found, initializing hooks...");

    try {
        auto kieroStatus = kiero::init(kiero::RenderType::D3D12);
        if (kieroStatus != kiero::Status::Success) {
            LOG_ERROR("Failed to initialize kiero");
            return false;
        }

        bool hooks_success = true;

        if (kiero::bind(54, (void **) &oExecuteCommandLists, hkExecuteCommandLists) != kiero::Status::Success) {
            LOG_ERROR("Failed to hook ExecuteCommandLists");
            hooks_success = false;
        }

        if (kiero::bind(58, (void **) &oSignal, hkSignal) != kiero::Status::Success) {
            LOG_ERROR("Failed to hook Signal");
            hooks_success = false;
        }

        if (kiero::bind(140, (void **) &oPresent, hkPresent) != kiero::Status::Success) {
            LOG_ERROR("Failed to hook Present");
            hooks_success = false;
        }

        if (kiero::bind(145, (void **) &oResizeBuffers, hkResizeBuffers) != kiero::Status::Success) {
            LOG_ERROR("Failed to hook ResizeBuffers");
            hooks_success = false;
        }

        if (!hooks_success) {
            LOG_ERROR("Failed to create one or more hooks");
            kiero::shutdown();
            return false;
        }

        LOG_INFO("D3D12 successfully hooked using kiero");
        return true;
    } catch (...) {
        LOG_ERROR("Exception during hook initialization");
        kiero::shutdown();
        return false;
    }
}

void ReleaseD3D12Hook() {
    kiero::shutdown();

    if (g_pd3dDevice) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_pd3dCommandQueue && g_fence && g_fenceEvent) {
        WaitForLastSubmittedFrame();
    }

    // Clean up render targets
    CleanupRenderTarget();

    // Release command allocators
    if (g_frameContext) {
        for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
            if (g_frameContext[i].CommandAllocator) {
                g_frameContext[i].CommandAllocator->Release();
                g_frameContext[i].CommandAllocator = nullptr;
            }
        }
        delete[] g_frameContext;
        g_frameContext = nullptr;
    }

    if (g_pd3dCommandList) {
        g_pd3dCommandList->Release();
        g_pd3dCommandList = nullptr;
    }

    if (g_pd3dCommandQueue) {
        g_pd3dCommandQueue->Release();
        g_pd3dCommandQueue = nullptr;
    }

    // Close handles before releasing resources
    if (g_fenceEvent) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }

    if (g_hSwapChainWaitableObject) {
        CloseHandle(g_hSwapChainWaitableObject);
        g_hSwapChainWaitableObject = nullptr;
    }

    if (g_pd3dRtvDescHeap) {
        g_pd3dRtvDescHeap->Release();
        g_pd3dRtvDescHeap = nullptr;
    }

    if (g_pd3dSrvDescHeap) {
        g_pd3dSrvDescHeap->Release();
        g_pd3dSrvDescHeap = nullptr;
    }

    if (g_fence) {
        g_fence->Release();
        g_fence = nullptr;
    }

    if (oWndProc && window) {
        SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR) oWndProc);
        oWndProc = nullptr;
    }

    g_pd3dDevice = nullptr;
    g_pSwapChain = nullptr;
    window = nullptr;

    NUM_BACK_BUFFERS = -1;
    g_frameIndex = 0;
    g_fenceValue = 0;
}
