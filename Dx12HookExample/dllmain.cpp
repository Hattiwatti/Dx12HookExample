#include <stdio.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <Windows.h>

#include "HookUtil.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

#define LOG(f_, ...) printf((f_), __VA_ARGS__); printf("\n");

static bool							s_initialized = false;
static IDXGISwapChain*				s_swapChain = nullptr;
static IDXGISwapChain3*				s_swapChain3 = nullptr;
static ID3D12Device*				s_dx12Device = nullptr;

static ID3D12DescriptorHeap*		g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap*		g_pd3dSrvDescHeap = nullptr;
static ID3D12CommandQueue*			g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList*	g_pd3dCommandList = nullptr;
static ID3D12Fence*					g_fence = nullptr;
static HANDLE                       g_fenceEvent = nullptr;
static UINT64                       g_fenceLastSignaledValue = 1;
static D3D12_CPU_DESCRIPTOR_HANDLE* g_mainRenderTargetDescriptor = nullptr;

struct FrameContext
{
	ID3D12CommandAllocator* CommandAllocator;
	UINT64                  FenceValue;
};

static FrameContext*				g_frameContexts = nullptr;
static ID3D12Resource**				g_backBuffers = nullptr;
static UINT							g_backBufferCount = 0;

WNDPROC								g_origWndProc = 0;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return TRUE;

	return CallWindowProc(g_origWndProc, hWnd, msg, wParam, lParam);
}

static void initializeDx12()
{
	// This is a bit of a question mark, might be a different offset for different versions.
	// Could possibly log the vtable pointer for ID3D12CommandQueue in mainThread() and then simply check for valid, readable pointers in a sensible range of the swapchain.
	// Idea #3: Hook CommandList::ResourceBarrier, check which command list creates transitions for any of the backbuffers, then also hook CommandQueue::ExecuteCommandLists
	// and find out which command queue is used to exectue that command list.
	size_t* pOffset = (size_t*)((__int64)s_swapChain + 0x160);
	*(&g_pd3dCommandQueue) = reinterpret_cast<ID3D12CommandQueue*>(*pOffset);
	printf("CommandQueue from SwapChain pointer at 0x%I64X\n", reinterpret_cast<__int64>(g_pd3dCommandQueue));

	s_swapChain->GetDevice(IID_PPV_ARGS(&s_dx12Device));
	if (!s_dx12Device)
	{
		LOG("Could not get ID3D12Device from IDXGISwapChain");
		return;
	}

	{
		DXGI_SWAP_CHAIN_DESC swapChainDesc{};
		s_swapChain->GetDesc(&swapChainDesc);

		LOG("DXGI_SWAP_CHAIN_DESC BufferCount %d", swapChainDesc.BufferCount);
		g_backBufferCount = swapChainDesc.BufferCount;
		g_backBuffers = new ID3D12Resource * [g_backBufferCount];

		for (UINT i = 0; i < g_backBufferCount; ++i)
		{
			s_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
			LOG("\tBackbuffer %d - 0x%I64X", i, reinterpret_cast<__int64>(g_backBuffers[i]));
		}
	}

	// Create descriptor heaps
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc{};
		desc.NumDescriptors = g_backBufferCount;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		desc.NodeMask = 1;

		if (FAILED(s_dx12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap))))
		{
			LOG("Failed to create DescriptorHeap for backbuffers");
			return;
		}

		g_mainRenderTargetDescriptor = new D3D12_CPU_DESCRIPTOR_HANDLE[g_backBufferCount];
		SIZE_T rtvDescriptorSize = s_dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
		for (UINT i = 0; i < g_backBufferCount; i++)
		{
			g_mainRenderTargetDescriptor[i] = rtvHandle;
			rtvHandle.ptr += rtvDescriptorSize;
		}

		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 1;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(s_dx12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap))))
		{
			LOG("Failed to create SRV DescriptorHeap");
			return;
		}
	}

	// Query SwapChain3 which can be used to get the current backbuffer index
	{
		s_swapChain->QueryInterface(IID_PPV_ARGS(&s_swapChain3));
		if (s_swapChain3)
		{
			UINT currentIndex = s_swapChain3->GetCurrentBackBufferIndex();
			LOG("Current backbuffer index: %d", currentIndex);
		}
		else
		{
			LOG("Could not query SwapChain to SwapChain3");
			return;
		}
	}

	// Create frame contexts
	{
		g_frameContexts = new FrameContext[g_backBufferCount];
		for (UINT i = 0; i < g_backBufferCount; ++i)
		{
			if (FAILED(s_dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContexts[i].CommandAllocator))))
			{
				LOG("Failed to create CommandAllocator for FrameContext %d", i);
				return;
			}
		}
	}

	// Create command list
	{
		if (s_dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContexts[0].CommandAllocator, NULL, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
			g_pd3dCommandList->Close() != S_OK)
		{
			LOG("Failed to create CommandList");
			return;
		}
	}

	// Create fence and fence event
	{
		if (s_dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
		{
			LOG("Failed to create Fence");
			return;
		}

		g_fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (g_fenceEvent == NULL)
		{
			LOG("Failed to create FenceEvent");
			return;
		}
	}

	// Create render target views
	{
		for (UINT i = 0; i < g_backBufferCount; ++i)
		{
			s_dx12Device->CreateRenderTargetView(g_backBuffers[i], NULL, g_mainRenderTargetDescriptor[i]);
		}
	}

	// Setup ImGui
	{
		HWND hwnd = FindWindowA("TR2NxApp", NULL);
		if (hwnd == NULL)
		{
			hwnd = FindWindowA(NULL, "Rise of the Tomb Raider v1.0 build 820.0_64");
			if (hwnd == NULL)
			{
				LOG("Could not get HWND");
			}
		}

		LOG("Game window handle 0x%X", reinterpret_cast<int>(hwnd));

		// Subclass the window with a new WndProc to catch messages
		g_origWndProc = (WNDPROC)SetWindowLongPtr(hwnd, -4, (LONG_PTR)&WndProc);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();

		// Setup Platform/Renderer bindings
		ImGui_ImplWin32_Init(hwnd);
		ImGui_ImplDX12_Init(s_dx12Device, g_backBufferCount,
			DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
			g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
			g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());
	}

	s_initialized = true;
}

static void drawImgui()
{
	const UINT backBufferIndex = s_swapChain3->GetCurrentBackBufferIndex();

	// Start the Dear ImGui frame
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	{
		static float f = 0.0f;
		static int counter = 0;

		ImGui::Begin("Hello, world!");    // Create a window called "Hello, world!" and append into it.

		ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)

		ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f

		if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
			counter++;
		ImGui::SameLine();
		ImGui::Text("counter = %d", counter);

		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		ImGui::End();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
	unsigned int renderTargetViewDescriptorSize;
	renderTargetViewHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
	renderTargetViewDescriptorSize = s_dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	if (backBufferIndex == 1)
	{
		renderTargetViewHandle.ptr += renderTargetViewDescriptorSize;
	}

	// Rendering
	FrameContext* frameCtxt = &g_frameContexts[0];
	frameCtxt->CommandAllocator->Reset();

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = g_backBuffers[backBufferIndex];
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

	g_pd3dCommandList->Reset(frameCtxt->CommandAllocator, NULL);
	g_pd3dCommandList->ResourceBarrier(1, &barrier);
	g_pd3dCommandList->OMSetRenderTargets(1, &renderTargetViewHandle, FALSE, NULL);
	g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	g_pd3dCommandList->ResourceBarrier(1, &barrier);
	g_pd3dCommandList->Close();

	g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList * const*)& g_pd3dCommandList);
}

static void waitForFence()
{
	// Signal and increment the fence value.
	UINT64 fenceToWaitFor = g_fenceLastSignaledValue;
	g_pd3dCommandQueue->Signal(g_fence, fenceToWaitFor);
	g_fenceLastSignaledValue++;

	// Wait until the GPU is done rendering.
	if (g_fence->GetCompletedValue() < fenceToWaitFor)
	{
		g_fence->SetEventOnCompletion(fenceToWaitFor, g_fenceEvent);
		WaitForSingleObject(g_fenceEvent, INFINITE);
	}
}

typedef DWORD(WINAPI* tIDXGISwapChain_Present)(IDXGISwapChain*, UINT, UINT);
tIDXGISwapChain_Present oSwapChain_Present = nullptr;
DWORD WINAPI hSwapChain_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	static bool s_presentCalled = false;
	if (!s_presentCalled)
	{
		s_presentCalled = true;
		LOG("IDXGISwapChain::Present called");

		s_swapChain = pSwapChain;
		initializeDx12();
	}

	if (s_initialized)
	{
		drawImgui();
		DWORD result = oSwapChain_Present(pSwapChain, SyncInterval, Flags);
		waitForFence();
		return result;
	}

	return oSwapChain_Present(pSwapChain, SyncInterval, Flags);
}

typedef DWORD(WINAPI* tIDXGISwapChain1_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
tIDXGISwapChain1_Present1 oSwapChain1_Present1 = nullptr;
DWORD WINAPI hSwapChain1_Present1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	static bool s_present1Called = false;
	if(!s_present1Called)
	{
		s_present1Called = true;
		LOG("IDXGISwapChain1::Present1 called");

		s_swapChain = dynamic_cast<IDXGISwapChain*>(pSwapChain);
		initializeDx12();
	}

	if (s_initialized)
	{
		DWORD result = oSwapChain1_Present1(pSwapChain, SyncInterval, Flags, pPresentParameters);
		waitForFence();
		return result;
	}

	return oSwapChain1_Present1(pSwapChain, SyncInterval, Flags, pPresentParameters);
}

LRESULT WINAPI TmpWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

FILE* pfstdin;
FILE* pfstdout;

static void mainThread()
{
	AllocConsole();
	freopen_s(&pfstdout, "CONOUT$", "w", stdout);
	freopen_s(&pfstdin, "CONIN$", "r", stdin);

	LOG("Hello World!");

	// Create temporary window, DX factories, devices, swapchain etc. to hook them
	WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, TmpWndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "A Fancy Temporary Window", NULL };
	RegisterClassExA(&wc);
	HWND hwnd = CreateWindowA(wc.lpszClassName, "A Fancy Temporary Window", WS_OVERLAPPEDWINDOW, 1, 1, 1, 1, NULL, NULL, wc.hInstance, NULL);

	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_0;

	IDXGIFactory4* pTmpDXGIFactory;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&pTmpDXGIFactory))))
	{
		LOG("Failed to create DXGIFactory");
		return;
	}

	ID3D12Device* pTmpDevice;
	if (FAILED(D3D12CreateDevice(NULL, featureLevel, IID_PPV_ARGS(&pTmpDevice))))
	{
		LOG("Failed to create D3D12Device");
		return;
	}

	ID3D12CommandQueue* pTmpCommandQueue;
	{
		D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
		commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		if (FAILED(pTmpDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&pTmpCommandQueue))))
		{
			LOG("Failed to create CommandQueue");
			return;
		}
	}

	IDXGISwapChain* pTmpSwapChain;
	{
		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
		swapChainDesc.BufferCount = 2;
		swapChainDesc.BufferDesc.Width = 0;
		swapChainDesc.BufferDesc.Height = 0;
		swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
		swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.OutputWindow = hwnd;
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.SampleDesc.Quality = 0;
		swapChainDesc.Windowed = TRUE;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

		if (FAILED(pTmpDXGIFactory->CreateSwapChain(pTmpCommandQueue, &swapChainDesc, &pTmpSwapChain)))
		{
			printf("Failed to create IDXGISwapChain1\n");
			return;
		}
	}

	// Hook it!
	__int64* pSwapChainVTable = *(__int64**)(pTmpSwapChain);
	__int64* pCommandQueueVTable = *(__int64**)(pTmpCommandQueue);

	__int64 pPresent = pSwapChainVTable[8];
	__int64 pPresent1 = pSwapChainVTable[22];
	__int64 pExecuteCommandLists = pCommandQueueVTable[10];

	CreateVTableHook("IDXGISwapChain::Present", (PDWORD64*)&pSwapChainVTable, hSwapChain_Present, 8, &oSwapChain_Present);

	//CreateHook("IDXGISwapChain::Present", pPresent, hSwapChain_Present, &oSwapChain_Present);
	//CreateHook("IDXGISwapChain1::Present1", pPresent1, hSwapChain_Present1, &oSwapChain_Present1);
	//CreateHook("ID3D12CommandQueue::ExecuteCommandLists", pExecuteCommandLists, hCommandQueue_ExecuteCommandLists, &oCommandQueue_ExecuteCommandLists);

	// Release temporary objects
	pTmpCommandQueue->Release();
	pTmpDevice->Release();
	pTmpSwapChain->Release();
	pTmpDXGIFactory->Release();

	CloseWindow(hwnd);
	DestroyWindow(hwnd);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)mainThread, 0, 0, 0);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

