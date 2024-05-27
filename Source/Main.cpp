#define NOMINMAX 1
#include <dxgiformat.h> // DXGI_FORMAT
#include <dxgi1_4.h>
#include <tchar.h>
#include <directx/d3dx12.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <Shlwapi.h>
#include <d3dcompiler.h>

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
	return Must(S_OK == res, errMsg);
}

using namespace DirectX;
using Matrix4x4 = DirectX::XMMATRIX;
using Vector4 = DirectX::XMVECTOR;
using Vector3 = DirectX::XMFLOAT3;
using Vector2 = DirectX::XMFLOAT2;

struct HelloTriangle
{
	static constexpr int BACK_BUFFER_COUNT = 3;
	struct {
		int Width = 1280;
		int Height = 720;
		HWND Handle = nullptr;
	} Window;
	D3D12_VIEWPORT Viewport;
	D3D12_RECT ScissorRect;
	ComPtr<ID3D12Device2> Device = nullptr;
	ComPtr<ID3D12CommandAllocator> CmdAllocators[BACK_BUFFER_COUNT]{};
	ComPtr<ID3D12CommandQueue> CmdQueue = nullptr;

	ComPtr<IDXGISwapChain3> SwapChain = nullptr;
	HANDLE SwapChainWaitableObject = nullptr;
	ComPtr<ID3D12Resource> SwapChainRTResources[BACK_BUFFER_COUNT] = {};

	ComPtr<ID3D12DescriptorHeap> InputTexturesHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> InputTextureSamplersHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> RTVHeap = nullptr;
	uint32_t RTVDescriptorSize = 0;

	ComPtr<ID3D12RootSignature> RootSignature = nullptr;
	ComPtr<ID3D12PipelineState> PipelineState = nullptr;
	ComPtr<ID3D12GraphicsCommandList> CmdList = nullptr;
	ComPtr<ID3D12Fence> Fence = nullptr;
	HANDLE FenceEvent = nullptr;
	UINT64 FenceValues[BACK_BUFFER_COUNT]{};
	uint32_t FrameIndex = 0;

	ComPtr<ID3D12Resource> VertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView;

	struct {
		ComPtr<ID3D12Resource> InputTexture;
		ComPtr<ID3D12Resource> OutputTexture;
		HANDLE InputHandle = nullptr;
		HANDLE OutputHandle = nullptr;
	} Shared;

	HelloTriangle(HWND windowHandle, int width, int height) : Window{ width, height, windowHandle },
		Viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)},
		ScissorRect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)}
	{
#ifdef DX12_ENABLE_DEBUG_LAYER
		ComPtr<ID3D12Debug> pdx12Debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
			pdx12Debug->EnableDebugLayer();
#endif

		Must(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)), "Unable to create D3D12 Device");

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
		Must(Device->CreateCommandQueue(&commandQueueDesc, __uuidof(ID3D12CommandQueue), (void**)&CmdQueue), "Unable to create CommandQueue");

		// Create Descriptor Heap for Render Target Views and two imported
		D3D12_DESCRIPTOR_HEAP_DESC shaderRtViewHeapDesc = {};
		shaderRtViewHeapDesc.NumDescriptors = 10;
		shaderRtViewHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		shaderRtViewHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&shaderRtViewHeapDesc, IID_PPV_ARGS(&InputTexturesHeap)), "Unable to create CBV_SRV_UAV DescriptorHeap");

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = 10;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		Must(Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&RTVHeap)), "Unable to create RTV DescriptorHeap");

		D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
		samplerHeapDesc.NumDescriptors = 10;
		samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Must(Device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&InputTextureSamplersHeap)), "Unable to create Sampler DescriptorHeap");


		CreateInputOutputSharedTextures();
		SetupSwapChain();
		SetupPipeline();
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
		Must(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)), "Unable to create DXGIFactory2");
		Must(dxgiFactory->CreateSwapChainForHwnd(CmdQueue.Get(), Window.Handle, &sd, nullptr, nullptr, &swapChain1));
		Must(swapChain1->QueryInterface(IID_PPV_ARGS(&SwapChain)));
		SwapChain->SetMaximumFrameLatency(3);
		SwapChainWaitableObject = SwapChain->GetFrameLatencyWaitableObject();
		FrameIndex = SwapChain->GetCurrentBackBufferIndex();

		auto rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		RTVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		for (int i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			Must(SwapChain->GetBuffer(i, IID_PPV_ARGS(&SwapChainRTResources[i])));
			Device->CreateRenderTargetView(SwapChainRTResources[i].Get(), nullptr, rtvHandle);
			rtvHandle.ptr += RTVDescriptorSize;

			Must(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAllocators[i])));
		}
	}

	void SetupPipeline()
	{
		Must(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAllocators[FrameIndex].Get(), PipelineState.Get(), IID_PPV_ARGS(&CmdList)), "Failed to create command list");

		std::vector<CD3DX12_ROOT_PARAMETER1> rootParams;
		CD3DX12_ROOT_PARAMETER1 rootParam = {};
		CD3DX12_DESCRIPTOR_RANGE1 range = CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		rootParam.InitAsDescriptorTable(1, &range);
		rootParams.push_back(rootParam);
		/*CD3DX12_DESCRIPTOR_RANGE1 range = CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
		rootParam = {};
		rootParam.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_VERTEX);
		rootParams.push_back(rootParam);*/

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
		Must(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signature, &error));
		Must(Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&RootSignature)), "Unable to create root signature");

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
				float2 texCoord : TEXCOORD;
			};
			VSOutput main(VSInput input)
			{
				VSOutput output;
				output.position = float4(input.position, 1.0f);
				output.color = input.color;
				output.texCoord = float2((input.position.x + 1.0f) / 2.0f, (1.0f - input.position.y) / 2.0f);
				return output;
			}
		)";
		constexpr const char* pixelShaderSource = R"(
		struct PSInput
		{
			float4 position : SV_POSITION; // Position of the vertex
			float4 color : COLOR;          // Color of the vertex
			float2 texCoord : TEXCOORD;    // Texture coordinates
		};

		Texture2D backgroundTexture : register(t0); // Texture unit 0
		SamplerState backgroundTextureSampler : register(s0); // Sampler state

		float4 main(PSInput input) : SV_TARGET
		{
			float4 sampledColor = backgroundTexture.Sample(backgroundTextureSampler, input.texCoord);

			// If input.color is not zero, use it; otherwise, use the sampled texture color
			if (input.color.r > 0 && input.color.g > 0 && input.color.b > 0)
			{
				return input.color;
			}
			else
			{
				return sampledColor;
			}
		}
		)";
		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;
		Must(D3DCompile(vertexShaderSource, strlen(vertexShaderSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vertexShader, &error), "Unable to compile vertex shader");
		Must(D3DCompile(pixelShaderSource, strlen(pixelShaderSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pixelShader, &error), "Unable to compile pixel shader");

		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
		psoDesc.pRootSignature = RootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;

		Must(Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&PipelineState)), "Failed to create a pipeline state");

		// Command lists are created in the recording state, but there is nothing
		// to record yet. The main loop expects it to be closed, so close it now.
		Must(CmdList->Close());

		CreateVertexBuffer();

		CreateFence();
	}

	void CreateInputOutputSharedTextures()
	{
		// Create a texture for the input
		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = 1280;
		textureDesc.Height = 720;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		Must(Device->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_SHARED,
			&textureDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&Shared.InputTexture)), "Failed to create input texture");

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		Device->CreateShaderResourceView(Shared.InputTexture.Get(), &srvDesc, InputTexturesHeap->GetCPUDescriptorHandleForHeapStart());

		// Create a texture for the output
		textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		Must(Device->CreateCommittedResource(
			&heapProp,
			D3D12_HEAP_FLAG_SHARED,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&Shared.OutputTexture)), "Failed to create output texture");

		// Creata shared handle for the input texture
		Must(Device->CreateSharedHandle(Shared.InputTexture.Get(), nullptr, GENERIC_ALL, nullptr, &Shared.InputHandle), "Failed to create shared handle for input texture");
		Must(Device->CreateSharedHandle(Shared.OutputTexture.Get(), nullptr, GENERIC_ALL, nullptr, &Shared.OutputHandle), "Failed to create shared handle for output texture");
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
			{ { 0.0f, 0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
			{ { 0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
			{ { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
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
			IID_PPV_ARGS(&VertexBuffer)), "Failed to create vertex buffer");

		// Copy the triangle data to the vertex buffer.
		UINT8* vertexDataBegin;
		CD3DX12_RANGE readRange(0, 0);
		Must(VertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&vertexDataBegin)), "Failed to map vertex buffer");
		memcpy(vertexDataBegin, triangleVertices, sizeof(triangleVertices));
		VertexBuffer->Unmap(0, nullptr);
		// Initialize the vertex buffer view.
		VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
		VertexBufferView.StrideInBytes = sizeof(Vertex);
		VertexBufferView.SizeInBytes = vertexBufferSize;
	}

	void CreateFence()
	{
		Must(Device->CreateFence(FenceValues[FrameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
		FenceValues[FrameIndex]++;

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
		Must(CmdQueue->Signal(Fence.Get(), FenceValues[FrameIndex]));

		// Wait until the fence has been processed.
		Must(Fence->SetEventOnCompletion(FenceValues[FrameIndex], FenceEvent));
		WaitForSingleObjectEx(FenceEvent, INFINITE, FALSE);

		// Increment the fence value for the current frame.
		FenceValues[FrameIndex]++;
	}
	
	void MoveToNextFrame()
	{
		const UINT64 currentFenceValue = FenceValues[FrameIndex];
		Must(CmdQueue->Signal(Fence.Get(), currentFenceValue));

		FrameIndex = SwapChain->GetCurrentBackBufferIndex();

		// If the next frame is not ready to be rendered yet, wait until it is ready.
		if (Fence->GetCompletedValue() < FenceValues[FrameIndex])
		{
			Must(Fence->SetEventOnCompletion(FenceValues[FrameIndex], FenceEvent));
			WaitForSingleObjectEx(FenceEvent, INFINITE, FALSE);
		}

		// Set the fence value for the next frame.
		FenceValues[FrameIndex] = currentFenceValue + 1;
	}

	void Render()
	{
		PopulateCommandList();

		ID3D12CommandList* ppCommandLists[] = { CmdList.Get() };
		CmdQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		Must(SwapChain->Present(1, 0));
		
		MoveToNextFrame();
	}

	void Destroy()
	{
		WaitForGpu();
		CloseHandle(FenceEvent);
	}

	void PopulateCommandList()
	{
		Must(CmdAllocators[FrameIndex]->Reset());

		Must(CmdList->Reset(CmdAllocators[FrameIndex].Get(), PipelineState.Get()));

		auto* heap = InputTexturesHeap.Get();
		CmdList->SetDescriptorHeaps(1, &heap);
		CmdList->SetGraphicsRootSignature(RootSignature.Get());

		CmdList->SetGraphicsRootDescriptorTable(0, InputTexturesHeap->GetGPUDescriptorHandleForHeapStart());

		CmdList->RSSetViewports(1, &Viewport);
		CmdList->RSSetScissorRects(1, &ScissorRect);
		
		auto bar1 = CD3DX12_RESOURCE_BARRIER::Transition(SwapChainRTResources[FrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		CmdList->ResourceBarrier(1, &bar1);

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(RTVHeap->GetCPUDescriptorHandleForHeapStart(), FrameIndex, RTVDescriptorSize);
		CmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

		// Record commands.
		const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
		CmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
		CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		CmdList->IASetVertexBuffers(0, 1, &VertexBufferView);
		CmdList->DrawInstanced(3, 1, 0, 0);

		auto bar2 = CD3DX12_RESOURCE_BARRIER::Transition(SwapChainRTResources[FrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		CmdList->ResourceBarrier(1, &bar2);
		CmdList->CopyResource(Shared.OutputTexture.Get(), SwapChainRTResources[FrameIndex].Get());

		// Indicate that the back buffer will now be used to present.
		auto bar3 = CD3DX12_RESOURCE_BARRIER::Transition(SwapChainRTResources[FrameIndex].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
		CmdList->ResourceBarrier(1, &bar3);

		Must(CmdList->Close());
	}
};

struct SampleEventDelegates : nos::app::IEventDelegates
{
	SampleEventDelegates(nos::app::IAppServiceClient* client, struct HelloTriangle* app) : Client(client), App(app) {}

	nos::app::IAppServiceClient* Client;
	HelloTriangle* App;
	nos::fb::UUID NodeId{};

	void OnAppConnected(const nos::fb::Node* appNode) override
	{
		std::cout << "Connected to Nodos" << std::endl;
		if (appNode)
			OnNodeImported(*appNode);
	}

	void OnNodeImported(nos::fb::Node const& appNode) override
	{
		NodeId = *appNode.id();
		auto inputTexDef = ExportSharedTexture(App->Shared.InputHandle, App->Shared.InputTexture.Get());
		auto outputTexDef = ExportSharedTexture(App->Shared.OutputHandle, App->Shared.OutputTexture.Get());
		flatbuffers::FlatBufferBuilder fbb;
		auto inPinId = GenerateId();
		auto outPinId = GenerateId();
		std::vector<uint8_t> inputPinBuf = nos::Buffer::From(inputTexDef);
		std::vector<flatbuffers::Offset<nos::fb::Pin>> pins = {
			nos::fb::CreatePinDirect(fbb, &inPinId, "Input", "nos.sys.vulkan.Texture", nos::fb::ShowAs::INPUT_PIN, nos::fb::CanShowAs::INPUT_PIN_ONLY, 0, 0, &inputPinBuf),
			nos::fb::CreatePinDirect(fbb, &outPinId, "Output", "nos.sys.vulkan.Texture", nos::fb::ShowAs::OUTPUT_PIN, nos::fb::CanShowAs::OUTPUT_PIN_ONLY, 0, 0, &inputPinBuf)
		};
		fbb.Finish(nos::CreatePartialNodeUpdateDirect(fbb, &NodeId, nos::ClearFlags::CLEAR_PINS, 0, &pins));
		nos::Buffer update = fbb.Release();
		Client->SendPartialNodeUpdate(*update.As<nos::PartialNodeUpdate>());
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
		ext.mutate_handle((u64)handle);
		D3D12_RESOURCE_DESC desc = texture->GetDesc();
		ext.mutate_allocation_size(App->Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes);
		ext.mutate_pid(getpid());
		def.unmanaged = false;
		def.unscaled = true;
		def.handle = 0;
		return def;
	}

	nos::fb::UUID GenerateId()
	{
		nos::fb::UUID id;
		std::array<uint8_t, 16> idBytes;
		for (int i = 0; i < 16; i++)
			idBytes[i] = rand() % 256;
		id.mutable_bytes()->CopyFromSpan(idBytes);
		return id;
	}

	void OnNodeUpdated(nos::fb::Node const& appNode) override {
		OnNodeImported(appNode);
	}
	void OnContextMenuRequested(nos::app::AppContextMenuRequest const& request) override {}
	void OnContextMenuCommandFired(nos::app::AppContextMenuAction const& action) override {}
	void OnNodeRemoved() override {}
	void OnPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset, uint64_t frameNumber) override {}
	void OnPinShowAsChanged(nos::fb::UUID const& pinId, nos::fb::ShowAs newShowAs) override {}
	void OnExecuteAppInfo(nos::app::AppExecuteInfo const* appExecuteInfo) override {}
	void OnFunctionCall(nos::fb::UUID const& nodeId, nos::fb::Node const& function) override {}
	void OnNodeSelected(nos::fb::UUID const& nodeId) override {}
	void OnConnectionClosed() override {}
	void OnStateChanged(nos::app::ExecutionState newState) override {}
	void OnConsoleCommand(nos::app::ConsoleCommand const* consoleCommand) override {}
	void OnConsoleAutoCompleteSuggestionRequest(nos::app::ConsoleAutoCompleteSuggestionRequest const* consoleAutoCompleteSuggestionRequest) override {}
	void OnLoadNodesOnPaths(nos::app::LoadNodesOnPaths const* loadNodesOnPathsRequest) override {}
	void OnCloseApp() override {}
	void OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart) override {}
};

int HelloTriangleMain()
{
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
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(window, &wmInfo);
	windowHandle = wmInfo.info.win.window;

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

	HelloTriangle app(windowHandle, windowWidth, windowHeight);

	auto eventDelegates = std::make_unique<SampleEventDelegates>(client, &app);
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
			app.Render();
		}
	}

	app.Destroy();

	SDL_DestroyWindow(window);
	SDL_Quit();

	client->UnregisterEventDelegates();
	pfnShutdownClient(client);

	return 0;
}

int main() {
	auto ret = HelloTriangleMain();

#ifdef DX12_ENABLE_DEBUG_LAYER
	if (ComPtr<IDXGIDebug1> pDebug = nullptr; SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
		pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
#endif

	return ret;
}
