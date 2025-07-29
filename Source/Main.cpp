// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#define NOMINMAX 1
#include <dxgiformat.h> // DXGI_FORMAT
#include <dxgi1_6.h>
#include <tchar.h>
#include <directx/d3dx12.h>
#include <DirectXMath.h>
#include <Shlwapi.h>
#include <d3dcompiler.h>

#define SDL_MAIN_HANDLED 1
#include <SDL2/SDL.h>
#include <SDL_syswm.h>
#include <wrl/client.h>
#include <comdef.h>
using Microsoft::WRL::ComPtr;

// stl
#include <iostream>
#include <filesystem>
#include <thread>
#include <fstream>

// Nodos
#include "CommonEvents_generated.h"
#include <nosFlatBuffersCommon.h>
#include <Nodos/AppAPI.h>
#include <Nodos/AppHelpers.hpp>
#include <nosVulkanSubsystem/Types_generated.h>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

#define DX12_ENABLE_DEBUG_LAYER

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

inline void Must(bool cond, const char* errMsg = "Unspecified")

{
	if (cond)
		return;
	std::cerr << "Error: " << errMsg << std::endl;
	std::cerr << "Details: " << GetLastError() << std::endl;
	throw;
}

inline void Must(HRESULT res, const char* errMsg = "Unspecified")
{
	if (S_OK == res)
		return;
	std::cerr << "Error: " << errMsg << std::endl;
	std::cerr << "Details: " << GetLastError() << std::endl;
	_com_error err(res);
	std::cerr << err.ErrorMessage() << std::endl;
	throw;
}

using namespace DirectX;
using Matrix4x4 = DirectX::XMMATRIX;
using Vector4 = DirectX::XMVECTOR;
using Vector3 = DirectX::XMFLOAT3;
using Vector2 = DirectX::XMFLOAT2;

struct HelloTriangle
{
	static constexpr int BACK_BUFFER_COUNT = 3;

	struct
	{
		int Width = 1280;
		int Height = 720;
		HWND Handle = nullptr;
	} Window;

	D3D12_VIEWPORT Viewport;
	D3D12_RECT ScissorRect;
	ComPtr<ID3D12Device2> Device = nullptr;
	ComPtr<ID3D12CommandAllocator> CmdAllocators[BACK_BUFFER_COUNT * 3]{};
	ComPtr<ID3D12CommandQueue> CmdQueue = nullptr;

	ComPtr<IDXGISwapChain3> SwapChain = nullptr;
	HANDLE SwapChainWaitableObject = nullptr;
	ComPtr<ID3D12Resource> SwapChainRTResources[BACK_BUFFER_COUNT] = {};

	enum class TextureType
	{
		NodosInputTexture = 0,
		NodosOutputTexture,
		IntermediateTexture,
		SrgbConversionOutputTexture,
		Count
	};

	ComPtr<ID3D12DescriptorHeap> TexturesHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> TextureSamplersHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> RTVHeap = nullptr;
	uint32_t RTVDescriptorSize = 0;

	ComPtr<ID3D12Resource> IntermediateTexture = nullptr;
	
	struct
	{
		ComPtr<ID3D12RootSignature> RootSignature = nullptr;
		ComPtr<ID3D12PipelineState> State = nullptr;
		ComPtr<ID3D12Resource> TriangleBuffer = nullptr;
		D3D12_VERTEX_BUFFER_VIEW TriangleBufferView {};
	} MainPipeline {};

	struct
	{
		ComPtr<ID3D12RootSignature> RootSignature = nullptr;
		ComPtr<ID3D12PipelineState> State = nullptr;
		ComPtr<ID3D12Resource> QuadBuffer = nullptr;
		D3D12_VERTEX_BUFFER_VIEW QuadBufferView {};
		ComPtr<ID3D12Resource> OutputTexture = nullptr;
	} SrgbConvPipeline {};
	
	ComPtr<ID3D12GraphicsCommandList> CmdList = nullptr;
	ComPtr<ID3D12Fence> Fence = nullptr;
	HANDLE FenceEvent = nullptr;
	UINT64 FenceValues[BACK_BUFFER_COUNT]{};
	uint32_t SwapChainFrameIndex = 0;

	struct Exported
	{
		ComPtr<ID3D12Resource> Texture;
		HANDLE TextureHandle = nullptr;
		ComPtr<ID3D12Fence> Fence = nullptr;
		HANDLE FenceHandle = nullptr;
		HANDLE FenceEvent = nullptr;
	};

	struct
	{
		Exported Input, Output;
	} Shared;

	std::mutex ExecutionMutex;
	std::condition_variable ExecutionCV;
	nos::app::ExecutionState ExecutionState = nos::app::ExecutionState::IDLE;
	std::optional<uint64_t> NodosFrameNumber = std::nullopt;

	uint64_t FrameCounter = 0;

	struct
	{
		std::queue<std::function<void()>> Queue;
		std::mutex Mutex;
	} Tasks;

	bool EnableVsync;

	HelloTriangle(HWND windowHandle, int width, int height, bool vsyncEnabled, std::optional<uint32_t> gpuIndex) :
		Window{width, height, windowHandle},
		Viewport{
			0.0f, 0.0f, static_cast<float>(width),
			static_cast<float>(height)
		},
		ScissorRect{
			0, 0, static_cast<LONG>(width),
			static_cast<LONG>(height)
		},
		EnableVsync(vsyncEnabled)
	{
#ifdef DX12_ENABLE_DEBUG_LAYER
		ComPtr<ID3D12Debug> pdx12Debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
			pdx12Debug->EnableDebugLayer();
#endif

		ComPtr<IDXGIFactory6> dxgiFactory;
		Must(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)), "Failed to create DXGIFactory");

		std::vector<ComPtr<IDXGIAdapter1>> adapters;
		ComPtr<IDXGIAdapter1> adapter;
		UINT index = 0;

		while (dxgiFactory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			// Skip software adapters
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				++index;
				continue;
			}

			std::wcout << L"[" << index << L"] " << desc.Description << std::endl;
			adapters.push_back(adapter);
			++index;
		}

		Must(!adapters.empty(), "No suitable adapter found.");

		uint32_t selectedAdapter = 0;
		if (adapters.size() > 1)
		{
			if (gpuIndex)
				selectedAdapter = *gpuIndex;
			else
			{
				std::cout << "Select GPU: ";
				std::cin >> selectedAdapter;
			}
		}

		std::wcout << "Selected GPU: " << selectedAdapter << std::endl;

		Must(!(selectedAdapter < 0 || selectedAdapter >= static_cast<uint32_t>(adapters.size())), "Invalid adapter selection.");

		Must(D3D12CreateDevice(adapters[selectedAdapter].Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)),
			"Unable to create D3D12 Device");

#ifdef DX12_ENABLE_DEBUG_LAYER
		if (pdx12Debug != nullptr)
		{
			ComPtr<ID3D12InfoQueue> pInfoQueue = nullptr;
			Device->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
		}
#endif
		D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
		commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		Must(Device->CreateCommandQueue(&commandQueueDesc, __uuidof(ID3D12CommandQueue), (void**)&CmdQueue),
			 "Unable to create CommandQueue");

		// Create Descriptor Heap for Render Target Views and two imported
		D3D12_DESCRIPTOR_HEAP_DESC shaderRtViewHeapDesc = {};
		shaderRtViewHeapDesc.NumDescriptors = 10;
		shaderRtViewHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		shaderRtViewHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&shaderRtViewHeapDesc, IID_PPV_ARGS(&TexturesHeap)),
			 "Unable to create CBV_SRV_UAV DescriptorHeap");

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = 10;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		Must(Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&RTVHeap)), "Unable to create RTV DescriptorHeap");

		D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
		samplerHeapDesc.NumDescriptors = 10;
		samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&TextureSamplersHeap)),
			 "Unable to create Sampler DescriptorHeap");

		CreateTextures();
		SetupSwapChain();
		SetupPipeline();
		SetupLinear2SrgbConversionPipeline();
		CreateFence();
	}

	void UpdateSyncState_GrpcThread(nos::app::ExecutionState newState)
	{
		std::lock_guard<std::mutex> lock(ExecutionMutex);
		ExecutionState = newState;
		if (newState == nos::app::ExecutionState::IDLE)
			NodosFrameNumber = 0;
		ExecutionCV.notify_all();
	}

	void UpdateSyncState(nos::app::ExecutionState newState)
	{
		std::cout << "UpdateSyncState: " << nos::app::EnumNameExecutionState(newState) << "\n";
		std::lock_guard<std::mutex> lock(ExecutionMutex);
		if (newState == nos::app::ExecutionState::IDLE)
			FrameCounter = 0;
	}

	void WaitOrSignalNodosFence(nos::fb::ShowAs showAs, uint64_t frameNumber, bool wait)
	{
		switch (showAs)
		{
		case nos::fb::ShowAs::INPUT_PIN:
			if (!Shared.Input.Fence.Get())
				return;
			if (wait)
				Must(CmdQueue->Wait(Shared.Input.Fence.Get(), 2 * frameNumber + 1));
			else
				Must(CmdQueue->Signal(Shared.Input.Fence.Get(), 2 * frameNumber + 2));
			break;
		case nos::fb::ShowAs::OUTPUT_PIN:
			if (!Shared.Output.Fence.Get())
				return;
			if (wait)
				Must(CmdQueue->Wait(Shared.Output.Fence.Get(), 2 * frameNumber));
			else
				Must(CmdQueue->Signal(Shared.Output.Fence.Get(), 2 * frameNumber + 1));
			break;
		default:
			break;
		}
	}

	void DestroyExternalSyncFence(Exported& exported)
	{
		if (exported.Fence)
		{
			CloseHandle(exported.FenceHandle);
			exported.Fence = nullptr;
		}
		if (exported.FenceEvent)
		{
			CloseHandle(exported.FenceEvent);
			exported.FenceEvent = nullptr;
		}
	}

	void RecreateExternalSyncFence(Exported& exported)
	{
		DestroyExternalSyncFence(exported);
		Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&exported.Fence));
		Must(Device->CreateSharedHandle(exported.Fence.Get(), 0, GENERIC_ALL, 0, &exported.FenceHandle));
		exported.FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (exported.FenceEvent == nullptr)
			Must(HRESULT_FROM_WIN32(GetLastError()));
	}

	void RecreateExternalSyncFences()
	{
		FrameCounter = 0;
		NodosFrameNumber = 0;
		RecreateExternalSyncFence(Shared.Input);
		RecreateExternalSyncFence(Shared.Output);
	}

	void DestroyExternalSyncFences()
	{
		DestroyExternalSyncFence(Shared.Input);
		DestroyExternalSyncFence(Shared.Output);
	}

	void SetupSwapChain()
	{
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
		ComPtr<IDXGISwapChain1> swapChain1 = nullptr;
		IDXGIFactory2* dxgiFactory = nullptr;
		uint32_t dxgiFactoryCreateFlags = 0;
#ifdef DX12_ENABLE_DEBUG_LAYER
		dxgiFactoryCreateFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
		Must(CreateDXGIFactory2(dxgiFactoryCreateFlags, IID_PPV_ARGS(&dxgiFactory)), "Unable to create DXGIFactory2");
		Must(dxgiFactory->CreateSwapChainForHwnd(CmdQueue.Get(), Window.Handle, &sd, nullptr, nullptr, &swapChain1));
		Must(swapChain1->QueryInterface(IID_PPV_ARGS(&SwapChain)));
		SwapChain->SetMaximumFrameLatency(3);
		SwapChainWaitableObject = SwapChain->GetFrameLatencyWaitableObject();
		SwapChainFrameIndex = SwapChain->GetCurrentBackBufferIndex();

		auto rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		RTVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		
		for (int i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			Must(SwapChain->GetBuffer(i, IID_PPV_ARGS(&SwapChainRTResources[i])));
			Device->CreateRenderTargetView(SwapChainRTResources[i].Get(), &rtvDesc, rtvHandle);
			rtvHandle.ptr += RTVDescriptorSize;

			for(int a = i * 3; a < i * 3 + 3; a++)
				Must(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAllocators[a])));
		}
		
		Device->CreateRenderTargetView(IntermediateTexture.Get(), &rtvDesc, rtvHandle); // IntermediateTexture's linear RTV
		rtvHandle.ptr += RTVDescriptorSize;
		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		Device->CreateRenderTargetView(SrgbConvPipeline.OutputTexture.Get(), &rtvDesc, rtvHandle);
	}

	void SetupPipeline()
	{
		Must(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAllocators[SwapChainFrameIndex * 3].Get(),
									   MainPipeline.State.Get(), IID_PPV_ARGS(&CmdList)), "Failed to create command list");

		std::vector<CD3DX12_ROOT_PARAMETER1> rootParams;
		CD3DX12_ROOT_PARAMETER1 rootParam = {};
		CD3DX12_DESCRIPTOR_RANGE1 range = CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		rootParam.InitAsDescriptorTable(1, &range);
		rootParams.push_back(rootParam);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
		// Create a static sampler
		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;

		rootSignatureDesc.Init_1_1(
			rootParams.size(),
			rootParams.data(),
			1,
			&sampler,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		Must(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signature,
												   &error));
		Must(Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
										 IID_PPV_ARGS(&MainPipeline.RootSignature)), "Unable to create root signature");

		constexpr const char* vertexShaderSource = R"(
			struct VSInput
			{
				float3 position : POSITION;
				float4 color : COLOR;
			};
			struct VSOutput
			{
				float4 position : SV_POSITION;
				float4 color : COLOR;
			};
			VSOutput main(VSInput input)
			{
				VSOutput output;
				output.position = float4(input.position, 1.0f);
				output.color = input.color;
				return output;
			}
		)";
		constexpr const char* pixelShaderSource = R"(
			struct PSInput
			{
				float4 position : SV_POSITION; // Position of the vertex
				float4 color : COLOR;          // Color of the vertex
			};

			float4 main(PSInput input) : SV_TARGET
			{
				return input.color;
			}
		)";
		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;
		Must(D3DCompile(vertexShaderSource, strlen(vertexShaderSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0,
						0, &vertexShader, &error), "Unable to compile vertex shader");
		Must(D3DCompile(pixelShaderSource, strlen(pixelShaderSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
						&pixelShader, &error), "Unable to compile pixel shader");

		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = {inputElementDescs, _countof(inputElementDescs)};
		psoDesc.pRootSignature = MainPipeline.RootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		auto& rt0Blend = psoDesc.BlendState.RenderTarget[0];
		rt0Blend.BlendEnable = true;
		rt0Blend.BlendOp = D3D12_BLEND_OP_ADD;
		rt0Blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		rt0Blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		rt0Blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt0Blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt0Blend.DestBlendAlpha = D3D12_BLEND_ZERO;
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;

		Must(Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&MainPipeline.State)),
			 "Failed to create a pipeline state");

		// Command lists are created in the recording state, but there is nothing
		// to record yet. The main loop expects it to be closed, so close it now.
		Must(CmdList->Close());

		CreateVertexBuffer();
	}

	void SetupLinear2SrgbConversionPipeline()
	{
		std::vector<CD3DX12_ROOT_PARAMETER1> rootParams;
		CD3DX12_ROOT_PARAMETER1 rootParam = {};
		CD3DX12_DESCRIPTOR_RANGE1 range = CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		rootParam.InitAsDescriptorTable(1, &range);
		rootParams.push_back(rootParam);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
		// Create a static sampler
		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;

		rootSignatureDesc.Init_1_1(
			rootParams.size(),
			rootParams.data(),
			1,
			&sampler,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		Must(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signature,
												   &error));
		Must(Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
										 IID_PPV_ARGS(&SrgbConvPipeline.RootSignature)),
			 "Unable to create root signature");

		constexpr const char* vertexShaderSource = R"(
					struct VSInput
					{
						float3 position : POSITION;
						float2 texCoord : TEXCOORD;
					};
					struct VSOutput
					{
						float4 position : SV_POSITION;
						float2 texCoord : TEXCOORD;
					};
					VSOutput main(VSInput input)
					{
						VSOutput output;
						output.position = float4(input.position, 1.0f);
						output.texCoord = input.texCoord;
						return output;
					}
				)";

		constexpr const char* pixelShaderSource = R"(
					Texture2D<float4> inputTexture : register(t0);
					SamplerState inputSampler : register(s0);
					struct PSInput
					{
						float4 position : SV_POSITION; // Position of the vertex
						float2 texCoord : TEXCOORD;
					};
					float4 main(PSInput input) : SV_TARGET
					{
						float4 color = inputTexture.Sample(inputSampler, input.texCoord);
						// color.rgb = pow(color.rgb, 1.0 / 2.2);
						return color;
					}
				)";

		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;
		Must(D3DCompile(vertexShaderSource, strlen(vertexShaderSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0,
						0, &vertexShader, &error), "Unable to compile vertex shader");
		Must(D3DCompile(pixelShaderSource, strlen(pixelShaderSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
						&pixelShader, &error), "Unable to compile pixel shader");

		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = {inputElementDescs, _countof(inputElementDescs)};
		psoDesc.pRootSignature = SrgbConvPipeline.RootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		psoDesc.SampleDesc.Count = 1;

		Must(Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&SrgbConvPipeline.State)),
			 "Failed to create a pipeline state");

		CreateQuad();
	}

	void CreateSharedTexture(D3D12_RESOURCE_DESC textureDesc, Exported& exported, D3D12_CPU_DESCRIPTOR_HANDLE dest)
	{
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_CLEAR_VALUE clear = { .Format = textureDesc.Format, .Color = {0.f, 0.f, 0.f, 1.f} };
		Must(Device->CreateCommittedResource(
				 &heapProp,
				 D3D12_HEAP_FLAG_SHARED,
				 &textureDesc,
				 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				 &clear,
				 IID_PPV_ARGS(&exported.Texture)), "Failed to create shared texture");

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		Device->CreateShaderResourceView(exported.Texture.Get(), &srvDesc,
										 dest);
		
		// Creata shared handle for the input texture
		Must(Device->CreateSharedHandle(exported.Texture.Get(), nullptr, GENERIC_ALL, nullptr,
										&exported.TextureHandle),
			 "Failed to create shared handle for shared texture");
	}

	void CreateTextures()
	{
		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = 1280;
		textureDesc.Height = 720;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		
		auto heapStart = TexturesHeap->GetCPUDescriptorHandleForHeapStart();
		auto tmpStart = heapStart;

		CreateSharedTexture(textureDesc, Shared.Input, { heapStart.ptr + UINT(TextureType::NodosInputTexture) * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) });
		CreateSharedTexture(textureDesc, Shared.Output, { heapStart.ptr + UINT(TextureType::NodosOutputTexture) * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) });
		Shared.Input.Texture->SetName(L"Shared Input");
		Shared.Output.Texture->SetName(L"Shared Output");

		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		Must(Device->CreateCommittedResource(&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&IntermediateTexture)));
		
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		Must(Device->CreateCommittedResource(&heapProp,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&SrgbConvPipeline.OutputTexture)));
		
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		Device->CreateShaderResourceView(IntermediateTexture.Get(), &srvDesc, { heapStart.ptr + UINT(TextureType::IntermediateTexture) * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) });
		IntermediateTexture->SetName(L"Intermediate Texture");
		Device->CreateShaderResourceView(SrgbConvPipeline.OutputTexture.Get(), &srvDesc, { heapStart.ptr + UINT(TextureType::SrgbConversionOutputTexture) * Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) });
		SrgbConvPipeline.OutputTexture->SetName(L"SRGB Conversion Output");
	}

	void CreateVertexBuffer()
	{
		struct Vertex
		{
			XMFLOAT3 Position;
			XMFLOAT4 Color;
		};
		Vertex triangleVertices[] =
		{
			{{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
			{{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
			{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}
		};
		const UINT vertexBufferSize = sizeof(triangleVertices);

		CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		Must(Device->CreateCommittedResource(
				 &heapProperties,
				 D3D12_HEAP_FLAG_NONE,
				 &bufferDesc,
				 D3D12_RESOURCE_STATE_GENERIC_READ,
				 nullptr,
				 IID_PPV_ARGS(&MainPipeline.TriangleBuffer)), "Failed to create vertex buffer");

		// Copy the triangle data to the vertex buffer.
		UINT8* vertexDataBegin;
		CD3DX12_RANGE readRange(0, 0);
		Must(MainPipeline.TriangleBuffer->Map(0, &readRange, reinterpret_cast<void**>(&vertexDataBegin)),
			 "Failed to map vertex buffer");
		memcpy(vertexDataBegin, triangleVertices, sizeof(triangleVertices));
		MainPipeline.TriangleBuffer->Unmap(0, nullptr);
		// Initialize the vertex buffer view.
		MainPipeline.TriangleBufferView.BufferLocation = MainPipeline.TriangleBuffer->GetGPUVirtualAddress();
		MainPipeline.TriangleBufferView.StrideInBytes = sizeof(Vertex);
		MainPipeline.TriangleBufferView.SizeInBytes = vertexBufferSize;
	}

	void CreateQuad()
	{
		struct Vertex
		{
			XMFLOAT3 Position;
			XMFLOAT2 TexCoord;
		};

		Vertex quadVertices[] =
		{
			{{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},  // Top-left
			{{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},   // Top-right
			{{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, // Bottom-left
			{{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, // Bottom-left
			{{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},   // Top-right
			{{1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}}   // Bottom-right
		};

		const UINT vertexBufferSize = sizeof(quadVertices);

		CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

		Must(Device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&SrgbConvPipeline.QuadBuffer)), "Failed to create vertex buffer");

		// Copy the quad data to the vertex buffer.
		UINT8* vertexDataBegin;
		CD3DX12_RANGE readRange(0, 0);
		Must(SrgbConvPipeline.QuadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&vertexDataBegin)),
			"Failed to map vertex buffer");
		memcpy(vertexDataBegin, quadVertices, sizeof(quadVertices));
		SrgbConvPipeline.QuadBuffer->Unmap(0, nullptr);

		// Initialize the vertex buffer view.
		SrgbConvPipeline.QuadBufferView.BufferLocation = SrgbConvPipeline.QuadBuffer->GetGPUVirtualAddress();
		SrgbConvPipeline.QuadBufferView.StrideInBytes = sizeof(Vertex);
		SrgbConvPipeline.QuadBufferView.SizeInBytes = vertexBufferSize;
	}

	void CreateFence()
	{
		Must(Device->CreateFence(FenceValues[SwapChainFrameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
		FenceValues[SwapChainFrameIndex]++;

		// Create an event handle to use for frame synchronization.
		FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FenceEvent == nullptr)
		{
			Must(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Wait for pending GPU work to complete.
	void WaitForGpu()
	{
		// Schedule a Signal command in the queue.
		Must(CmdQueue->Signal(Fence.Get(), FenceValues[SwapChainFrameIndex]));

		// Wait until the fence has been processed.
		Must(Fence->SetEventOnCompletion(FenceValues[SwapChainFrameIndex], FenceEvent));
		WaitForSingleObjectEx(FenceEvent, INFINITE, FALSE);

		// Increment the fence value for the current frame.
		FenceValues[SwapChainFrameIndex]++;
	}

	void OnFrameRenderBegin()
	{
		/*
			Wait for input fence to be signalled to framenumber * 2 + 1
			Begin command list
			Copy Input to Intermediate
			Execute command list
			Signal input fence to framenumber * 2 + 2
		*/
		WaitOrSignalNodosFence(nos::fb::ShowAs::INPUT_PIN, FrameCounter, true);
		BeginCommandListFor(CommandAllocatorType::INPUT_COPIES);

		CD3DX12_RESOURCE_BARRIER bars[2];
		bars[0] = CD3DX12_RESOURCE_BARRIER::Transition(Shared.Input.Texture.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		bars[1] = CD3DX12_RESOURCE_BARRIER::Transition(IntermediateTexture.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST);
		CmdList->ResourceBarrier(2, bars);
		CmdList->CopyResource(IntermediateTexture.Get(), Shared.Input.Texture.Get());
		bars[0] = CD3DX12_RESOURCE_BARRIER::Transition(Shared.Input.Texture.Get(),
						D3D12_RESOURCE_STATE_COPY_SOURCE,
						D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		bars[1] = CD3DX12_RESOURCE_BARRIER::Transition(IntermediateTexture.Get(),
						D3D12_RESOURCE_STATE_COPY_DEST,
						D3D12_RESOURCE_STATE_RENDER_TARGET);
		CmdList->ResourceBarrier(2, bars);
		CloseAndExecuteCommandList();
		WaitOrSignalNodosFence(nos::fb::ShowAs::INPUT_PIN, FrameCounter, false);
	}

	void MoveToNextFrame()
	{
		/*
			Wait for output fence to be signalled to framenumber * 2
			Begin command list
			Copy Intermediate to Output
			Execute command list
			Signal output fence to framenumber * 2 + 1
			Increase framenumber

			Swapchain
		*/
		WaitOrSignalNodosFence(nos::fb::ShowAs::OUTPUT_PIN, FrameCounter, true);
		BeginCommandListFor(CommandAllocatorType::OUTPUT_COPIES);

		// Copy Intermediate to Output
		CD3DX12_RESOURCE_BARRIER bars[2];
		bars[0] = CD3DX12_RESOURCE_BARRIER::Transition(IntermediateTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		bars[1] = CD3DX12_RESOURCE_BARRIER::Transition(Shared.Output.Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		CmdList->ResourceBarrier(2, bars);
		CmdList->CopyResource(Shared.Output.Texture.Get(), IntermediateTexture.Get());
		bars[0] = CD3DX12_RESOURCE_BARRIER::Transition(IntermediateTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		bars[1] = CD3DX12_RESOURCE_BARRIER::Transition(Shared.Output.Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		CmdList->ResourceBarrier(2, bars);
		CloseAndExecuteCommandList();
		WaitOrSignalNodosFence(nos::fb::ShowAs::OUTPUT_PIN, FrameCounter, false);
		//Send execution completed
		
		
		FrameCounter++;

		const UINT64 currentFenceValue = FenceValues[SwapChainFrameIndex];
		Must(CmdQueue->Signal(Fence.Get(), currentFenceValue));

		SwapChainFrameIndex = SwapChain->GetCurrentBackBufferIndex();

		// If the next frame is not ready to be rendered yet, wait until it is ready.
		if (Fence->GetCompletedValue() < FenceValues[SwapChainFrameIndex])
		{
			Must(Fence->SetEventOnCompletion(FenceValues[SwapChainFrameIndex], FenceEvent));
			WaitForSingleObjectEx(FenceEvent, INFINITE, FALSE);
		}

		// Set the fence value for the next frame.
		FenceValues[SwapChainFrameIndex] = currentFenceValue + 1;
	}

	// Returns processed nodos frame count if it did
	std::optional<uint64_t> Render()
	{
		{
			std::unique_lock lock(Tasks.Mutex);
			while (!Tasks.Queue.empty())
			{
				Tasks.Queue.front()();
				Tasks.Queue.pop();
			}
		}

		// Wait for NodosFrameNumber > FrameCounter
		{
			std::unique_lock lock(ExecutionMutex);
			ExecutionCV.wait(lock, [&] { return FrameCounter <= NodosFrameNumber.value_or(0) || ExecutionState == nos::app::ExecutionState::IDLE; });
			if(ExecutionState == nos::app::ExecutionState::IDLE)
				return std::nullopt;
		}

		OnFrameRenderBegin();

		RenderMainPipeline();

		Must(SwapChain->Present(EnableVsync ? 1 : 0, 0));
		uint64_t processedFrameCounter = FrameCounter;
		MoveToNextFrame();
		return processedFrameCounter;
	}

	void Destroy()
	{
		WaitForGpu();
		DestroyExternalSyncFences();
		CloseHandle(FenceEvent);
	}

	enum class CommandAllocatorType
	{
		INPUT_COPIES,
		MAIN_PIPELINE,
		OUTPUT_COPIES
	};

	ID3D12CommandAllocator* GetCommandAllocator(CommandAllocatorType type)
	{
		switch (type)
		{
		case CommandAllocatorType::INPUT_COPIES:
			return CmdAllocators[SwapChainFrameIndex * 3].Get();
		case CommandAllocatorType::MAIN_PIPELINE:
			return CmdAllocators[SwapChainFrameIndex * 3 + 1].Get();
		case CommandAllocatorType::OUTPUT_COPIES:
			return CmdAllocators[SwapChainFrameIndex * 3 + 2].Get();
		default:
			return nullptr;
		}
	}

	void BeginCommandListFor(CommandAllocatorType type)
	{
		Must(GetCommandAllocator(type)->Reset());
		Must(CmdList->Reset(GetCommandAllocator(type), MainPipeline.State.Get()));
	}

	void CloseAndExecuteCommandList()
	{
		Must(CmdList->Close());
		ID3D12CommandList* ppCommandLists[] = {CmdList.Get()};
		CmdQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	}

	void RenderMainPipeline()
	{
		BeginCommandListFor(CommandAllocatorType::MAIN_PIPELINE);

		auto* heap = TexturesHeap.Get();
		CmdList->SetDescriptorHeaps(1, &heap);
		CmdList->RSSetViewports(1, &Viewport);
		CmdList->RSSetScissorRects(1, &ScissorRect);
		
		// Main Pipeline with triangle
		{
			CD3DX12_CPU_DESCRIPTOR_HANDLE intermediateRTVHandle(RTVHeap->GetCPUDescriptorHandleForHeapStart(), BACK_BUFFER_COUNT, RTVDescriptorSize);
			// Already in RT state
			CmdList->OMSetRenderTargets(1, &intermediateRTVHandle, FALSE, nullptr);

			CmdList->SetGraphicsRootSignature(MainPipeline.RootSignature.Get());

			CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CmdList->IASetVertexBuffers(0, 1, &MainPipeline.TriangleBufferView);

			CmdList->DrawInstanced(3, 1, 0, 0);

			CD3DX12_RESOURCE_BARRIER bar = CD3DX12_RESOURCE_BARRIER::Transition(IntermediateTexture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			CmdList->ResourceBarrier(1, &bar);
		}

		// Linear -> SRGB conversion for window
		{
			CmdList->SetPipelineState(SrgbConvPipeline.State.Get());
			CmdList->SetGraphicsRootSignature(SrgbConvPipeline.RootSignature.Get());
			CD3DX12_GPU_DESCRIPTOR_HANDLE intermediateTextureStart(TexturesHeap->GetGPUDescriptorHandleForHeapStart(), UINT(TextureType::IntermediateTexture), // Shared Output
										  Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
			CmdList->SetGraphicsRootDescriptorTable(0, intermediateTextureStart);
			
			CD3DX12_CPU_DESCRIPTOR_HANDLE srgbRtvHandle(RTVHeap->GetCPUDescriptorHandleForHeapStart(), BACK_BUFFER_COUNT + 1, // SRGB RTV
										  RTVDescriptorSize);

			auto bar = CD3DX12_RESOURCE_BARRIER::Transition(SrgbConvPipeline.OutputTexture.Get(),
													   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
													   D3D12_RESOURCE_STATE_RENDER_TARGET);
			CmdList->ResourceBarrier(1, &bar);
			
			CmdList->OMSetRenderTargets(1, &srgbRtvHandle, FALSE, nullptr);
			CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CmdList->IASetVertexBuffers(0, 1, &SrgbConvPipeline.QuadBufferView);
			
			CmdList->DrawInstanced(6, 1, 0, 0);

			CD3DX12_RESOURCE_BARRIER bars[2];
			bars[0] = CD3DX12_RESOURCE_BARRIER::Transition(SwapChainRTResources[SwapChainFrameIndex].Get(),
														   D3D12_RESOURCE_STATE_PRESENT,
														   D3D12_RESOURCE_STATE_COPY_DEST);
			bars[1] = CD3DX12_RESOURCE_BARRIER::Transition(SrgbConvPipeline.OutputTexture.Get(),
														   D3D12_RESOURCE_STATE_RENDER_TARGET,
														   D3D12_RESOURCE_STATE_COPY_SOURCE);
			CmdList->ResourceBarrier(2, bars);
			CmdList->CopyResource(SwapChainRTResources[SwapChainFrameIndex].Get(), SrgbConvPipeline.OutputTexture.Get());

			bar =  CD3DX12_RESOURCE_BARRIER::Transition(SrgbConvPipeline.OutputTexture.Get(),
													   D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			CmdList->ResourceBarrier(1, &bar);

			// Indicate that the back buffer will now be used to present.
			bar = CD3DX12_RESOURCE_BARRIER::Transition(SwapChainRTResources[SwapChainFrameIndex].Get(),
													   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
			CmdList->ResourceBarrier(1, &bar);
			
		}

		CloseAndExecuteCommandList();
	}

	void EnqueueTask(std::function<void()> fun)
	{
		std::unique_lock lock(Tasks.Mutex);
		Tasks.Queue.push(std::move(fun));
	}
};

struct SampleEventDelegates : nos::app::AppEventDelegates
{
	SampleEventDelegates(nosAppServiceClient* client, HelloTriangle* app) : Client(client), App(app)
	{
	}

	nosAppServiceClient* Client;
	HelloTriangle* App;
	nos::fb::UUID NodeId{};

	void SendSyncSemaphores()
	{
		uint64_t inputSemaphore = (uint64_t)App->Shared.Input.FenceHandle;
		uint64_t outputSemaphore = (uint64_t)App->Shared.Output.FenceHandle;
		flatbuffers::FlatBufferBuilder mb;
		auto offset = nos::CreateAppEventOffset(
			mb, nos::app::CreateSetSyncSemaphores(mb, &NodeId, _getpid(), inputSemaphore, outputSemaphore));
		mb.Finish(offset);
		auto buf = mb.Release();
		auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
		Client->Send(Client->ServiceHandle, root);
	}

	void OnAppConnected(const nos::fb::Node* appNode)
	{
		std::cout << "Connected to Nodos" << std::endl;
		if (appNode)
			OnNodeImported(*appNode);
	}

	void OnNodeImported(nos::fb::Node const& appNode)
	{
		NodeId = *appNode.id();
		auto inputTexDef = ExportSharedTexture(App->Shared.Input.TextureHandle, App->Shared.Input.Texture.Get());
		auto outputTexDef = ExportSharedTexture(App->Shared.Output.TextureHandle, App->Shared.Output.Texture.Get());
		flatbuffers::FlatBufferBuilder fbb;
		auto inPinId = GenerateId();
		auto outPinId = GenerateId();
		std::vector<uint8_t> inputPinBuf = nos::Buffer::From(inputTexDef);
		std::vector<uint8_t> outputPinBuf = nos::Buffer::From(outputTexDef);
		std::vector pins = {
			nos::fb::CreatePinDirect(fbb, &inPinId, "Input", "nos.sys.vulkan.Texture", nos::fb::ShowAs::INPUT_PIN,
									 nos::fb::CanShowAs::INPUT_PIN_ONLY, 0, &inputPinBuf),
			nos::fb::CreatePinDirect(fbb, &outPinId, "Output", "nos.sys.vulkan.Texture", nos::fb::ShowAs::OUTPUT_PIN,
									 nos::fb::CanShowAs::OUTPUT_PIN_ONLY, 0, &outputPinBuf)
		};
		fbb.Finish(nos::CreatePartialNodeUpdateDirect(fbb, &NodeId,
													  nos::ClearFlags::CLEAR_PINS | nos::ClearFlags::CLEAR_NODES,
													  0, &pins, 0, 0, 0, 0, 0, 0, 0,
													  nos::fb::CreateNodeOrphanStateDirect(fbb, nos::fb::NodeOrphanStateType::ACTIVE, "")));
		nos::Buffer update = fbb.Release();
		Client->SendPartialNodeUpdate(Client->ServiceHandle, update.As<nos::PartialNodeUpdate>());
	}

	nos::sys::vulkan::TTexture ExportSharedTexture(HANDLE handle, ID3D12Resource* texture)
	{
		nos::sys::vulkan::TTexture def;
		def.width = 1280;
		def.height = 720;
		def.format = nos::sys::vulkan::Format::R8G8B8A8_UNORM;
		def.usage = nos::sys::vulkan::ImageUsage::SAMPLED;
		auto& ext = def.external_memory;
		ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE);
		ext.mutate_handle((uint64_t)handle);
		D3D12_RESOURCE_DESC desc = texture->GetDesc();
		ext.mutate_allocation_size(App->Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes);
		ext.mutate_pid(_getpid());
		def.unmanaged = false;
		def.unscaled = true;
		def.handle = 0;
		return def;
	}

	static nos::fb::UUID GenerateId()
	{
		nos::fb::UUID id;
		std::array<uint8_t, 16> idBytes;
		for (int i = 0; i < 16; i++)
			idBytes[i] = rand() % 256;
		id.mutable_bytes()->CopyFromSpan(idBytes);
		return id;
	}

	void OnNodeUpdated(nos::fb::Node const& appNode)
	{
		OnNodeImported(appNode);
	}

	void OnStateChanged(nos::app::ExecutionState newState)
	{
		App->UpdateSyncState_GrpcThread(newState);
		App->EnqueueTask([this, newState]
			{
				if (newState == nos::app::ExecutionState::SYNCED)
				{
					App->RecreateExternalSyncFences();
					SendSyncSemaphores();
				}
				else
					App->DestroyExternalSyncFences();
				App->UpdateSyncState(newState);
			});
	}

	void OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart)
	{
		std::unique_lock<std::mutex> lock(App->ExecutionMutex);
		if (appExecuteStart->reset())
			App->NodosFrameNumber = std::nullopt;
		else
			App->NodosFrameNumber = appExecuteStart->frame_counter();
		std::cout << "Received ExecuteStart: " << appExecuteStart->frame_counter() << std::endl;
		App->ExecutionCV.notify_all();
	}

	void HandleEvent(const nos::app::EngineEvent* event) override
	{
		using namespace nos::app;
		switch (event->event_type())
		{
		case EngineEventUnion::AppConnectedEvent: {
			OnAppConnected(event->event_as<AppConnectedEvent>()->node());
			break;
		}
		case EngineEventUnion::FullNodeUpdate: {
			OnNodeUpdated(*event->event_as<nos::FullNodeUpdate>()->node());
			break;
		}
		case EngineEventUnion::NodeImported: {
			OnNodeImported(*event->event_as<nos::app::NodeImported>()->node());
			break;
		}
		case EngineEventUnion::StateChanged: {
			OnStateChanged(event->event_as<nos::app::StateChanged>()->state());
			break;
		}
		case EngineEventUnion::AppExecuteStart: {
			OnExecuteStart(event->event_as<nos::app::AppExecuteStart>());
			break;
		}
		default:
			break;
		}
	}
	void OnConnectionClosed() override 
	{
		{
			std::unique_lock lock(App->ExecutionMutex);
			App->ExecutionState = nos::app::ExecutionState::IDLE;
			App->ExecutionCV.notify_all();
		}
		App->EnqueueTask([this]
			{
				App->DestroyExternalSyncFences();
				App->UpdateSyncState(nos::app::ExecutionState::IDLE);
			});
	}
};

// Utility to check if a file exists
bool FileExists(const std::string& path) {
	std::ifstream f(path.c_str());
	return f.good();
}

// Helper to run a command and capture its stdout (replaces _popen)
std::string RunCommandAndCapture(const std::string& cmd) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi = {};
    // CreateProcessA needs a modifiable buffer
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    BOOL success = CreateProcessA(
        NULL, cmdline.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(hWrite); // Parent doesn't need write end

    std::string output;
    if (success) {
        char buffer[256];
        DWORD read;
        while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0) {
            buffer[read] = '\0';
            output += buffer;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);
    return output;
}

// Helper to run nodos.exe and get SDK path from JSON output (manual parsing, no JSON lib)
std::string GetSdkPathFromNosman(const std::string& bundleRoot, const std::string& appSdkVersion) {
	std::string sdkPath;
	std::string nodosExe = bundleRoot + "\\nodos.exe";
	if (!FileExists(nodosExe)) 
		return sdkPath;

	std::string cmd = "\"" + nodosExe + "\" --workspace \"" + bundleRoot + "\" sdk-info \"" + appSdkVersion + "\" process";
	std::string jsonStr = RunCommandAndCapture(cmd);
	if (jsonStr.empty())
		return sdkPath;

	const char* key = "\"path\"";
	size_t keyPos = jsonStr.find(key);
	if (keyPos != std::string::npos) {
		size_t colon = jsonStr.find(':', keyPos);
		if (colon != std::string::npos) {
			size_t firstQuote = jsonStr.find('"', colon + 1);
			if (firstQuote != std::string::npos) {
				size_t secondQuote = jsonStr.find('"', firstQuote + 1);
				if (secondQuote != std::string::npos) {
					sdkPath = jsonStr.substr(firstQuote + 1, secondQuote - firstQuote - 1);
				}
			}
		}
	}
	if (sdkPath.empty())
		std::cerr << "Failed to parse SDK path from command output." << std::endl;
	return sdkPath;
}

int HelloTriangleMain(
	int windowWidth,
	int windowHeight,
	std::optional<uint32_t> gpuIndex,
	uint64_t sleepMs,
	bool vsync,
	const std::string& nodosSdkDllPath // new parameter
)
{
	SDL_WindowFlags window_flags =
		(SDL_WindowFlags)(SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);

	// Use passed windowWidth and windowHeight
	SDL_Init(SDL_INIT_VIDEO);
	SDL_Window* window = SDL_CreateWindow(
		"Sample DX12 App", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		windowWidth, windowHeight, window_flags);
	if (!window)
	{
		auto error = SDL_GetError();
		std::cout << "Failed to create window: " << error << std::endl;
		return 1;
	}

	HWND windowHandle = nullptr;
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(window, &wmInfo);
	windowHandle = wmInfo.info.win.window;

	// Initialize Nodos SDK
	FN_CheckSDKCompatibility pfnCheckSDKCompatibility = nullptr;
	FN_MakeAppServiceClient pfnMakeAppServiceClient = nullptr;
	FN_ShutdownClient pfnShutdownClient = nullptr;

	std::cout << "Using Nodos SDK DLL at: " << nodosSdkDllPath << std::endl;
	HMODULE sdkModule = LoadLibraryA(nodosSdkDllPath.c_str());
	Must(sdkModule, ("Failed to load Nodos SDK DLL: " + nodosSdkDllPath).c_str());
	pfnCheckSDKCompatibility = (FN_CheckSDKCompatibility)GetProcAddress(
		sdkModule, "CheckSDKCompatibility");
	pfnMakeAppServiceClient = (FN_MakeAppServiceClient)GetProcAddress(sdkModule, "MakeAppServiceClient");
	pfnShutdownClient = (FN_ShutdownClient)GetProcAddress(sdkModule, "ShutdownClient");

	Must(pfnCheckSDKCompatibility && pfnMakeAppServiceClient && pfnShutdownClient, "Failed to load Nodos SDK functions");

	Must(pfnCheckSDKCompatibility(NOS_APPLICATION_SDK_VERSION_MAJOR, NOS_APPLICATION_SDK_VERSION_MINOR,
		NOS_APPLICATION_SDK_VERSION_PATCH), "Incompatible Nodos SDK version");

	nosApplicationInfo appInfo{
		.AppKey = "Sample-DX12-App",
		.AppName = "Sample DX12 App"
	};
	nosAppServiceClient* client = pfnMakeAppServiceClient("localhost:50053", &appInfo);

	Must(client, "Failed to create App Service Client");

	HelloTriangle app(windowHandle, windowWidth, windowHeight, vsync, gpuIndex);

	auto eventDelegates = std::make_unique<SampleEventDelegates>(client, &app);
	client->RegisterEventDelegates(client->ServiceHandle, &eventDelegates.get()->Delegates);

	// Main loop
	SDL_Event event;
	bool running = true;
	while (running)
	{
		while (!client->IsConnected(client->ServiceHandle))
		{
			std::cout << "Trying to connect to Nodos..." << std::endl;
			client->TryConnect(client->ServiceHandle);
			if (!client->IsConnected(client->ServiceHandle))
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		SDL_PumpEvents();
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_QUIT)
			{
				running = false;
				break;
			}
		}
		// Add sleep if requested
		if (sleepMs > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
		if (std::optional<uint64_t> processedFrameNum = app.Render())
		{
			flatbuffers::FlatBufferBuilder fbb;
			client->Send(client->ServiceHandle, nos::CreateAppEvent(fbb, nos::app::CreateExecutionCompleted(fbb, &eventDelegates->NodeId, *processedFrameNum)));
		}
	}

	app.Destroy();

	SDL_DestroyWindow(window);
	SDL_Quit();

	uint64_t frameCounter = app.FrameCounter;
	flatbuffers::FlatBufferBuilder fbb;
	client->Send(client->ServiceHandle, nos::CreateAppEvent(fbb, nos::app::CreateAppConnectionClosed(fbb, frameCounter)));
	client->UnregisterEventDelegates(client->ServiceHandle);
	pfnShutdownClient(client);

	return 0;
}

int main(int argc, char** argv)
{
	uint64_t sleepMs = 0;
	std::optional<uint32_t> gpuIndex;
	int windowWidth = 1280;
	int windowHeight = 720;
	bool vsync = true;
	std::string nodosSdkDllPath;

	if (argc > 1)
	{
		for (int i = 1; i < argc; i++)
		{
			if (strcmp(argv[i], "--render-loop-sleep-ms") == 0 && i + 1 < argc)
				sleepMs = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc)
				gpuIndex = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
				windowWidth = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
				windowHeight = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--no-vsync") == 0)
				vsync = false;
			else if (strcmp(argv[i], "--nodos-sdk-dll") == 0 && i + 1 < argc)
				nodosSdkDllPath = argv[++i];
			else
				std::cerr << "Unknown argument: " << argv[i] << std::endl;
		}
	}

	char exePath[MAX_PATH];
	Must(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "Failed to get executable path.");
	std::filesystem::path exeDir = std::filesystem::absolute(exePath).parent_path();
	std::filesystem::path sdkDllCandidate = exeDir / "nosAppSDK.dll";
	nodosSdkDllPath = sdkDllCandidate.string();

	// Try to find nosAppSDK.dll using nodos.exe if still not found
	if (nodosSdkDllPath.empty() || !FileExists(nodosSdkDllPath)) {
		// Try to find bundle root (assume two levels up from exeDir: Samples/nos.sample.dxapp/<version>/Binaries)
		std::filesystem::path bundleRoot = exeDir;
		for (int i = 0; i < 4; ++i)
			bundleRoot = bundleRoot.parent_path();
		// Use App SDK version, not engine version
		std::string appSdkVersion = "18.0"; // use correct App SDK version
		std::string sdkPath = GetSdkPathFromNosman(bundleRoot.string(), appSdkVersion);
		if (!sdkPath.empty()) {
			std::string candidate = sdkPath + "\\bin\\nosAppSDK.dll";
			if (FileExists(candidate))
				nodosSdkDllPath = candidate;
		}
	}

	if (nodosSdkDllPath.empty() || !FileExists(nodosSdkDllPath)){
		const char* sdkDir = std::getenv("NODOS_SDK_DIR");
		if (sdkDir) {
			std::string candidate = std::string(sdkDir) + "/bin/nosAppSDK.dll";
			if (FileExists(candidate)) {
				nodosSdkDllPath = candidate;
			}
		}
	}

#ifdef NODOS_APP_SDK_DLL
	if (nodosSdkDllPath.empty() || !FileExists(nodosSdkDllPath)) {
		const char* macroPath = NODOS_APP_SDK_DLL;
		if (macroPath && FileExists(macroPath)) {
			nodosSdkDllPath = macroPath;
		}
	}
#endif

	while (nodosSdkDllPath.empty() || !FileExists(nodosSdkDllPath)) {
		std::cout << "Enter path to Nodos SDK DLL: ";
		std::getline(std::cin, nodosSdkDllPath);
		if (!FileExists(nodosSdkDllPath)) {
			std::cout << "File does not exist: " << nodosSdkDllPath << std::endl;
			nodosSdkDllPath.clear();
		}
	}

	// Pass parameters to HelloTriangleMain
	auto ret = HelloTriangleMain(windowWidth, windowHeight, gpuIndex, sleepMs, vsync, nodosSdkDllPath);

#ifdef DX12_ENABLE_DEBUG_LAYER
	if (ComPtr<IDXGIDebug1> pDebug = nullptr; SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
		pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
#endif

	return ret;
}
