#define NOMINMAX 1
#include <dxgiformat.h> // DXGI_FORMAT
#include <dxgi1_4.h>
#include <tchar.h>
#include <directx/d3dx12.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <Shlwapi.h>

#define SDL_MAIN_HANDLED 1
#include <SDL2/SDL.h>
#include <SDL_syswm.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// stl
#include <iostream>
#include <filesystem>
#include <thread>

// Nodos
#include "CommonEvents_generated.h"
#include <nosFlatBuffersCommon.h>
#include <Nodos/AppAPI.h>

inline void Must(bool cond, const char* errMsg = "Unspecified")

{
	if (cond)
		return;
	std::cerr << "Error: " << errMsg << std::endl;
	std::cerr << "Details: " << GetLastError() << std::endl;
	exit(1);
}

inline void Must(HRESULT res, const char* errMsg = "Unspecified")
{
	return Must(S_OK == res, errMsg);
}

using namespace DirectX;
using Matrix4x4 = DirectX::XMMATRIX;
using Vector4 = DirectX::XMVECTOR;
using Vector3 = DirectX::XMFLOAT3;
using Vector2 = DirectX::XMFLOAT2;

struct SampleEventDelegates : nos::app::IEventDelegates
{
	SampleEventDelegates(nos::app::IAppServiceClient* client) : Client(client) {}

	nos::app::IAppServiceClient* Client;
	nos::fb::UUID NodeId{};

	void OnAppConnected(const nos::fb::Node* appNode) override
	{
		std::cout << "Connected to Nodos" << std::endl;
		if (appNode)
			NodeId = *appNode->id();
	}
	void OnNodeUpdated(nos::fb::Node const& appNode) override {}
	void OnContextMenuRequested(nos::app::AppContextMenuRequest const& request) override {}
	void OnContextMenuCommandFired(nos::app::AppContextMenuAction const& action) override {}
	void OnNodeRemoved() override {}
	void OnPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset, uint64_t frameNumber) override {}
	void OnPinShowAsChanged(nos::fb::UUID const& pinId, nos::fb::ShowAs newShowAs) override {}
	void OnExecuteAppInfo(nos::app::AppExecuteInfo const* appExecuteInfo) override {}
	void OnFunctionCall(nos::fb::UUID const& nodeId, nos::fb::Node const& function) override {}
	void OnNodeSelected(nos::fb::UUID const& nodeId) override {}
	void OnNodeImported(nos::fb::Node const& appNode) override {}
	void OnConnectionClosed() override {}
	void OnStateChanged(nos::app::ExecutionState newState) override {}
	void OnConsoleCommand(nos::app::ConsoleCommand const* consoleCommand) override {}
	void OnConsoleAutoCompleteSuggestionRequest(nos::app::ConsoleAutoCompleteSuggestionRequest const* consoleAutoCompleteSuggestionRequest) override {}
	void OnLoadNodesOnPaths(nos::app::LoadNodesOnPaths const* loadNodesOnPathsRequest) override {}
	void OnCloseApp() override {}
	void OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart) override {}
};

struct HelloTriangle
{
	struct {
		int Width = 1280;
		int Height = 720;
		HWND Handle = nullptr;
	} Window;
	ComPtr<ID3D12Device2> Device = nullptr;
	ComPtr<ID3D12CommandAllocator> CmdAllocator = nullptr;
	ComPtr<ID3D12CommandQueue> CmdQueue = nullptr;

	ComPtr<IDXGISwapChain3> SwapChain = nullptr;
	HANDLE SwapChainWaitableObject = nullptr;
	static constexpr int BACK_BUFFER_COUNT = 3;
	ComPtr<ID3D12Resource> SwapChainRTResources[BACK_BUFFER_COUNT] = {};

	ComPtr<ID3D12DescriptorHeap> InputTexturesHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> InputTextureSamplersHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> RTVHeap = nullptr;

	HelloTriangle(HWND windowHandle, int width, int height) : Window{ width, height, windowHandle }
	{
		Must(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)), "Unable to create D3D12 Device");

		Must(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&CmdAllocator), "Unable to create CommandAllocator");
		
		D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
		commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		Must(Device->CreateCommandQueue(&commandQueueDesc, __uuidof(ID3D12CommandQueue), (void**)&CmdQueue), "Unable to create CommandQueue");

		// Create Descriptor Heap for Render Target Views and two imported
		D3D12_DESCRIPTOR_HEAP_DESC shaderRtViewHeapDesc = {};
		shaderRtViewHeapDesc.NumDescriptors = 10;
		shaderRtViewHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		shaderRtViewHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&shaderRtViewHeapDesc, IID_PPV_ARGS(&InputTexturesHeap)), "Unable to create DescriptorHeap");

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = 10;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&RTVHeap)), "Unable to create DescriptorHeap");

		D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
		samplerHeapDesc.NumDescriptors = 10;
		samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&InputTextureSamplersHeap)), "Unable to create Sampler DescriptorHeap");

		SetupSwapChain();
		SetupPipeline();
	}

	void SetupSwapChain()
	{
		// Get the DXGI factory used to create the swap chain.
		IDXGIFactory2* dxgiFactory = nullptr;
		Must(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&dxgiFactory), "Unable to create DXGIFactory2");
		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.BufferCount = BACK_BUFFER_COUNT;
		sd.Width = Window.Width;
		sd.Height = Window.Height;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.Stereo = FALSE;
		ComPtr<IDXGIFactory4> dxgiFactory = nullptr;
		ComPtr<IDXGISwapChain1> swapChain1 = nullptr;
		Must(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
		Must(dxgiFactory->CreateSwapChainForHwnd(CmdQueue.Get(), Window.Handle, &sd, nullptr, nullptr, &swapChain1));
		Must(swapChain1->QueryInterface(IID_PPV_ARGS(&SwapChain)));
		SwapChain->SetMaximumFrameLatency(3);
		SwapChainWaitableObject = SwapChain->GetFrameLatencyWaitableObject();

		auto rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		for (int i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			Must(SwapChain->GetBuffer(i, IID_PPV_ARGS(&SwapChainRTResources[i])));
			Device->CreateRenderTargetView(SwapChainRTResources[i].Get(), nullptr, rtvHandle);
			rtvHandle.ptr += Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		}
	}

	void SetupPipeline()
	{
	
	}
};

int main() {
	SDL_WindowFlags window_flags =
		(SDL_WindowFlags)(SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);

	int windowWidth = 1280;
	int windowHeight = 720;
	SDL_Init(SDL_INIT_VIDEO);
	SDL_Window* window = SDL_CreateWindow(
		"Sample DX12 App", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		windowWidth, windowHeight, window_flags);
	if (!window) {
		auto error = SDL_GetError();
		std::cout << "Failed to create window: " << error << std::endl;
		return 1;
	}

	HWND windowHandle = nullptr;
	SDL_SysWMinfo wm_info;
	SDL_VERSION(&wm_info.version);
	SDL_GetWindowWMInfo(window, &wm_info);
	windowHandle = wm_info.info.win.window;

	ComPtr<ID3D12Device2> device = nullptr;
	Must(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "Unable to create D3D12 Device");

	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	Must(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, reinterpret_cast<void*>(&options), sizeof(options)), "D3D12_FEATURE_D3D12_OPTIONS is not supported");

	ComPtr<ID3D12CommandAllocator> cmdAllocator = nullptr;
	Must(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&cmdAllocator), "Unable to create CommandAllocator");

	ComPtr<ID3D12CommandQueue> cmdQueue = nullptr;
	D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
	commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	Must(device->CreateCommandQueue(&commandQueueDesc, __uuidof(ID3D12CommandQueue), (void**)&cmdQueue), "Unable to create CommandQueue");

	

	// Initialize Nodos SDK
	nos::app::FN_CheckSDKCompatibility* pfnCheckSDKCompatibility = nullptr;
	nos::app::FN_MakeAppServiceClient* pfnMakeAppServiceClient = nullptr;
	nos::app::FN_ShutdownClient* pfnShutdownClient = nullptr;

	HMODULE sdkModule = LoadLibrary(NODOS_APP_SDK_DLL);
	if (sdkModule) {
		pfnCheckSDKCompatibility = (nos::app::FN_CheckSDKCompatibility*)GetProcAddress(sdkModule, "CheckSDKCompatibility");
		pfnMakeAppServiceClient = (nos::app::FN_MakeAppServiceClient*)GetProcAddress(sdkModule, "MakeAppServiceClient");
		pfnShutdownClient = (nos::app::FN_ShutdownClient*)GetProcAddress(sdkModule, "ShutdownClient");
	}
	else {
		std::cerr << "Failed to load Nodos SDK" << std::endl;
		return -1;
	}

	if (!pfnCheckSDKCompatibility || !pfnMakeAppServiceClient || !pfnShutdownClient) {
		std::cerr << "Failed to load Nodos SDK functions" << std::endl;
		return -1;
	}

	if (!pfnCheckSDKCompatibility(NOS_APPLICATION_SDK_VERSION_MAJOR, NOS_APPLICATION_SDK_VERSION_MINOR, NOS_APPLICATION_SDK_VERSION_PATCH)) {
		std::cerr << "Incompatible Nodos SDK version" << std::endl;
		return -1;
	}

	nos::app::IAppServiceClient* client = pfnMakeAppServiceClient("localhost:50053", nos::app::ApplicationInfo{
		.AppKey = "Sample-DX12-App",
		.AppName = "Sample DX12 App"
		});

	if (!client) {
		std::cerr << "Failed to create App Service Client" << std::endl;
		return -1;
	}
	// TODO: Shutdown client

	auto eventDelegates = std::make_unique<SampleEventDelegates>(client);
	client->RegisterEventDelegates(eventDelegates.get());

	client->TryConnect();
	while (!client->IsConnected()) {
		std::cout << "Trying to connect to Nodos..." << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(1));
		client->TryConnect();
	}

	// Main loop
	SDL_Event event;
	bool running = true;
	while (running) {
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				running = false;
				break;
			}
			// Render triangle on top a texture
			
		}
	}

	SDL_DestroyWindow(window);
	SDL_Quit();

	client->UnregisterEventDelegates();
	pfnShutdownClient(client);
	return 0;
}
