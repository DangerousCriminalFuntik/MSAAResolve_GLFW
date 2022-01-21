#include <cstdint>
#include <iostream>
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <DirectXMath.h>

#include "DXHelpers.h"

using namespace Microsoft::WRL;

#pragma comment(lib, "glfw3.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

uint32_t windowWidth = 1280;
uint32_t windowHeight = 720;

// Function prototypes
static void error_callback(int error, const char* message)
{
	printf("GLFW error[%d]: %s\n", error, message);
#if _WIN32
	__debugbreak();
#endif
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int modes)
{
	if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		glfwSetWindowShouldClose(window, 1);
}

constexpr auto CubeVertexShaderSource = R"(
cbuffer cb : register(b0)
{
	float4x4 TransformMatrix;
};

float4 VS(float3 Position : POSITION) : SV_Position
{
	return mul(float4(Position, 1.0f), TransformMatrix);
})";

constexpr auto FSQuadVertexShaderSource = R"(
float4 VS(uint VertexID : SV_VertexID) : SV_Position
{
	return float4(-1.0f + 2.0f * (VertexID % 2), 1.0f - 2.0f * (VertexID / 2), 0.0f, 1.0f);
})";

constexpr auto FSQuadPixelShaderSource = R"(
Texture2D<float> DepthBufferTexture : register(t0);

float4 PS(float4 Position : SV_Position) : SV_Target
{
	float PixelDepth = DepthBufferTexture.Load(int3(Position.xy, 0)).x;
	return float4(PixelDepth == 0.0f ? 1.0f : 0.0f, PixelDepth == 1.0f ? 1.0f : 0.0f, (PixelDepth > 0.0f) && (PixelDepth < 1.0f) ? 1.0f : 0.0f, 1.0f);
})";

void CompileShader(const char* ShaderSource, const char* ShaderName, const char* EntryPoint, const char* ShaderModel, ComPtr<ID3DBlob>& ShaderByteCodeBlob)
{
	ComPtr<ID3DBlob> ErrorBlob;
	HRESULT hr = D3DCompile(ShaderSource, strlen(ShaderSource), ShaderName, nullptr, nullptr, EntryPoint, ShaderModel, D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &ShaderByteCodeBlob, &ErrorBlob);

	if (FAILED(hr) && ErrorBlob.Get())
	{
		system("PAUSE.EXE");
		ExitProcess(0);
	}
}

int main(int argc, char* argv[])
{
	glfwSetErrorCallback(error_callback);

	if (!glfwInit())
		return -1;

	// Screen size
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	const uint32_t screenW = (uint32_t)mode->width;
	const uint32_t screenH = (uint32_t)mode->height;
	if (windowWidth > screenW)
		windowWidth = screenW;
	if (windowHeight > screenH)
		windowHeight = screenH;

	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "MSAA Resolve Test", nullptr, nullptr);
	if (!window)
	{
		std::wcout << "Failed to create GLFW window\n";
		glfwTerminate();
		return -1;
	}

	int32_t x = (screenW - windowWidth) >> 1;
	int32_t y = (screenH - windowHeight) >> 1;
	glfwSetWindowPos(window, x, y);

	// Sample loading
	printf("Loading...\n");

	std::string CommandLine(argv[0]);

	bool DebugMode = CommandLine.find("-dxdebug") != -1;

	ComPtr<IDXGIFactory6> Factory;
	SAFE_DX(CreateDXGIFactory2(DebugMode ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(Factory.ReleaseAndGetAddressOf())));

	ComPtr<IDXGIAdapter> Adapter;
	
	if (size_t Index = CommandLine.find("-adapterindex"); Index != -1)
	{
		int AdapterIndex = 0;
		auto AdapterIndexStr = CommandLine.substr(Index, CommandLine.find(' ', Index));

		sscanf_s(AdapterIndexStr.c_str(), "-adapterindex=%d", &AdapterIndex);

		SAFE_DX(Factory->EnumAdapters(AdapterIndex, &Adapter));
	}
	else if (size_t Index = CommandLine.find("-adaptervendor"); Index != -1)
	{
		auto AdapterVendorStr = CommandLine.substr(Index, CommandLine.find(' ', Index));

		char AdapterVendor[512];
		wchar_t wAdapterVendor[512];

		sscanf_s(AdapterVendorStr.c_str(), "-adaptervendor=%s", AdapterVendor, 512);

		for (int i = 0; i <= strlen(AdapterVendor); ++i)
			wAdapterVendor[i] = (wchar_t)AdapterVendor[i];

		int AdapterIndex = 0;

		while (Factory->EnumAdapters(AdapterIndex, (IDXGIAdapter**)&Adapter) != DXGI_ERROR_NOT_FOUND)
		{
			DXGI_ADAPTER_DESC AdapterDesc;
			SAFE_DX(Adapter->GetDesc(&AdapterDesc));

			if (std::wstring(AdapterDesc.Description).find(wAdapterVendor) != -1) break;

			++AdapterIndex;
		}

		if (Factory->EnumAdapters(AdapterIndex, (IDXGIAdapter**)&Adapter) == DXGI_ERROR_NOT_FOUND)
		{
			std::wcout << L"Не найдено графического адаптера с заданным производителем" << std::endl;
			ExitProcess(-1);
		}
	}
	else
	{
		SAFE_DX(Factory->EnumAdapters(0, &Adapter));
	}	

	DXGI_ADAPTER_DESC AdapterDesc;
	SAFE_DX(Adapter->GetDesc(&AdapterDesc));
	std::wcout << AdapterDesc.Description << std::endl;

	if (DebugMode)
	{
		ComPtr<ID3D12Debug1> DebugInterface;
		SAFE_DX(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugInterface)));
		DebugInterface->EnableDebugLayer();
		DebugInterface->SetEnableGPUBasedValidation(true);
	}

	ComPtr<ID3D12Device> Device;
	SAFE_DX(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(Device.ReleaseAndGetAddressOf())));

	D3D12_FEATURE_DATA_D3D12_OPTIONS2 FeatureOptions{};
	SAFE_DX(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &FeatureOptions, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2)));

	if (FeatureOptions.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED)
		std::wcout << L"D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED" << std::endl;
	else if (FeatureOptions.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1)
		std::wcout << L"D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1" << std::endl;
	else if (FeatureOptions.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_2)
		std::wcout << L"D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_2" << std::endl;

	ComPtr<ID3D12CommandQueue> CommandQueue;
	ComPtr<ID3D12CommandAllocator> CommandAllocators[2];
	ComPtr<ID3D12GraphicsCommandList> CommandList;
	ComPtr<ID3D12GraphicsCommandList1> CommandList1;

	D3D12_COMMAND_QUEUE_DESC CommandQueueDesc{};
	CommandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	CommandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	SAFE_DX(Device->CreateCommandQueue(&CommandQueueDesc, IID_PPV_ARGS(CommandQueue.ReleaseAndGetAddressOf())));

	SAFE_DX(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(CommandAllocators[0].ReleaseAndGetAddressOf())));
	SAFE_DX(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(CommandAllocators[1].ReleaseAndGetAddressOf())));

	SAFE_DX(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(CommandList.ReleaseAndGetAddressOf())));
	SAFE_DX(CommandList->Close());

	SAFE_DX(CommandList->QueryInterface<ID3D12GraphicsCommandList1>(CommandList1.ReleaseAndGetAddressOf()));

	DXGI_SWAP_CHAIN_DESC1 SwapChainDesc{};
	SwapChainDesc.BufferCount = 2;
	SwapChainDesc.Width = windowWidth;
	SwapChainDesc.Height = windowHeight;
	SwapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;	
	SwapChainDesc.SampleDesc.Count = 1;
	
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsChainDesc{};
	fsChainDesc.Windowed = TRUE;

	ComPtr<IDXGISwapChain3> SwapChain;
	ComPtr<IDXGISwapChain1> swapChain1;
	SAFE_DX(Factory->CreateSwapChainForHwnd(CommandQueue.Get(), glfwGetWin32Window(window), &SwapChainDesc, &fsChainDesc, nullptr, swapChain1.GetAddressOf()));
	SAFE_DX(swapChain1.As(&SwapChain));

	ComPtr<ID3D12Fence> FrameFences[2];
	SAFE_DX(Device->CreateFence(1, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(FrameFences[0].ReleaseAndGetAddressOf())));
	SAFE_DX(Device->CreateFence(1, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(FrameFences[1].ReleaseAndGetAddressOf())));

	HANDLE FrameEvent = nullptr;
	FrameEvent = CreateEvent(nullptr, FALSE, FALSE, L"FrameEvent");

	ComPtr<ID3D12DescriptorHeap> RTDescriptorHeap;
	ComPtr<ID3D12DescriptorHeap> DSDescriptorHeap;
	ComPtr<ID3D12DescriptorHeap> CBSRUADescriptorHeap;

	D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc;
	DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	DescriptorHeapDesc.NodeMask = 0;
	DescriptorHeapDesc.NumDescriptors = 2;
	DescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

	SAFE_DX(Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(RTDescriptorHeap.ReleaseAndGetAddressOf())));

	DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	DescriptorHeapDesc.NodeMask = 0;
	DescriptorHeapDesc.NumDescriptors = 1;
	DescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

	SAFE_DX(Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(DSDescriptorHeap.ReleaseAndGetAddressOf())));

	DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	DescriptorHeapDesc.NodeMask = 0;
	DescriptorHeapDesc.NumDescriptors = 2;
	DescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

	SAFE_DX(Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(CBSRUADescriptorHeap.ReleaseAndGetAddressOf())));

	ComPtr<ID3D12Resource> BackBufferTextures[2];
	D3D12_CPU_DESCRIPTOR_HANDLE BackBufferTexturesRTVs[2];

	SAFE_DX(SwapChain->GetBuffer(0, IID_PPV_ARGS(BackBufferTextures[0].GetAddressOf())));
	SAFE_DX(SwapChain->GetBuffer(1, IID_PPV_ARGS(BackBufferTextures[1].GetAddressOf())));

	D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
	RTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

	BackBufferTexturesRTVs[0].ptr = RTDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;
	BackBufferTexturesRTVs[1].ptr = BackBufferTexturesRTVs[0].ptr + Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	Device->CreateRenderTargetView(BackBufferTextures[0].Get(), &RTVDesc, BackBufferTexturesRTVs[0]);
	Device->CreateRenderTargetView(BackBufferTextures[1].Get(), &RTVDesc, BackBufferTexturesRTVs[1]);

	ComPtr<ID3D12Resource> DepthBufferTexture;
	ComPtr<ID3D12Resource> ResolvedDepthBufferTexture;

	D3D12_RESOURCE_DESC ResourceDesc;
	ResourceDesc.Alignment = 0;
	ResourceDesc.DepthOrArraySize = 1;
	ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	ResourceDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	ResourceDesc.Height = windowHeight;
	ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ResourceDesc.MipLevels = 1;
	ResourceDesc.SampleDesc.Count = 8;
	ResourceDesc.SampleDesc.Quality = 0;
	ResourceDesc.Width = windowWidth;

	D3D12_HEAP_PROPERTIES HeapProperties;
	HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	HeapProperties.CreationNodeMask = 0;
	HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
	HeapProperties.VisibleNodeMask = 0;

	D3D12_CLEAR_VALUE ClearValue;
	ClearValue.DepthStencil.Depth = 1.0f;
	ClearValue.DepthStencil.Stencil = 0;
	ClearValue.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

	SAFE_DX(Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, &ClearValue, IID_PPV_ARGS(DepthBufferTexture.ReleaseAndGetAddressOf())));

	D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc;
	DSVDesc.Flags = D3D12_DSV_FLAG_NONE;
	DSVDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;

	D3D12_CPU_DESCRIPTOR_HANDLE DepthBufferTextureDSV;
	DepthBufferTextureDSV.ptr = DSDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;

	Device->CreateDepthStencilView(DepthBufferTexture.Get(), &DSVDesc, DepthBufferTextureDSV);

	ResourceDesc.Alignment = 0;
	ResourceDesc.DepthOrArraySize = 1;
	ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	ResourceDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	ResourceDesc.Height = windowHeight;
	ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ResourceDesc.MipLevels = 1;
	ResourceDesc.SampleDesc.Count = 1;
	ResourceDesc.SampleDesc.Quality = 0;
	ResourceDesc.Width = windowWidth;

	HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	HeapProperties.CreationNodeMask = 0;
	HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
	HeapProperties.VisibleNodeMask = 0;

	ClearValue.DepthStencil.Depth = 1.0f;
	ClearValue.DepthStencil.Stencil = 0;
	ClearValue.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

	SAFE_DX(Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue, IID_PPV_ARGS(ResolvedDepthBufferTexture.ReleaseAndGetAddressOf())));

	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc;
	SRVDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SRVDesc.Texture2D.MipLevels = 1;
	SRVDesc.Texture2D.MostDetailedMip = 0;
	SRVDesc.Texture2D.PlaneSlice = 0;
	SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

	D3D12_CPU_DESCRIPTOR_HANDLE ResolvedDepthBufferTextureSRV;
	ResolvedDepthBufferTextureSRV.ptr = CBSRUADescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr + Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	Device->CreateShaderResourceView(ResolvedDepthBufferTexture.Get(), &SRVDesc, ResolvedDepthBufferTextureSRV);

	ComPtr<ID3D12Resource> VertexBuffer;
	ComPtr<ID3D12Resource> IndexBuffer;
	ComPtr<ID3D12Resource> ConstantBuffer;

	ResourceDesc.Alignment = 0;
	ResourceDesc.DepthOrArraySize = 1;
	ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	ResourceDesc.Height = 1;
	ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ResourceDesc.MipLevels = 1;
	ResourceDesc.SampleDesc.Count = 1;
	ResourceDesc.SampleDesc.Quality = 0;

	HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	HeapProperties.CreationNodeMask = 0;
	HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
	HeapProperties.VisibleNodeMask = 0;

	ResourceDesc.Width = sizeof(DirectX::XMFLOAT3) * 8;
	SAFE_DX(Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(VertexBuffer.ReleaseAndGetAddressOf())));

	ResourceDesc.Width = sizeof(WORD) * 36;
	SAFE_DX(Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(IndexBuffer.ReleaseAndGetAddressOf())));

	ResourceDesc.Width = 256;
	SAFE_DX(Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(ConstantBuffer.ReleaseAndGetAddressOf())));

	void* BufferData;
	SAFE_DX(VertexBuffer->Map(0, nullptr, &BufferData));

	DirectX::XMFLOAT3 *Vertices = (DirectX::XMFLOAT3*)BufferData;
	Vertices[0] = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
	Vertices[1] = DirectX::XMFLOAT3(-1.0f, 1.0f, 1.0f);
	Vertices[2] = DirectX::XMFLOAT3(1.0f, -1.0f, 1.0f);
	Vertices[3] = DirectX::XMFLOAT3(-1.0f, -1.0f, 1.0f);
	Vertices[4] = DirectX::XMFLOAT3(1.0f, 1.0f, -1.0f);
	Vertices[5] = DirectX::XMFLOAT3(-1.0f, 1.0f, -1.0f);
	Vertices[6] = DirectX::XMFLOAT3(1.0f, -1.0f, -1.0f);
	Vertices[7] = DirectX::XMFLOAT3(-1.0f, -1.0f, -1.0f);

	VertexBuffer->Unmap(0, nullptr);

	SAFE_DX(IndexBuffer->Map(0, nullptr, &BufferData));

	WORD* Indices = (WORD*)BufferData;
	Indices[0] = 5; Indices[1] = 4; Indices[2] = 7;
	Indices[3] = 7; Indices[4] = 4; Indices[5] = 6;

	Indices[6] = 0; Indices[7] = 1; Indices[8] = 2;
	Indices[9] = 2; Indices[10] = 1; Indices[11] = 3;
	
	Indices[12] = 4; Indices[13] = 0; Indices[14] = 6;
	Indices[15] = 6; Indices[16] = 0; Indices[17] = 2;
	
	Indices[18] = 1; Indices[19] = 5; Indices[20] = 3;
	Indices[21] = 3; Indices[22] = 5; Indices[23] = 7;
	
	Indices[24] = 1; Indices[25] = 0; Indices[26] = 5;
	Indices[27] = 5; Indices[28] = 0; Indices[29] = 4;

	Indices[30] = 2; Indices[31] = 3; Indices[32] = 6;
	Indices[33] = 6; Indices[34] = 3; Indices[35] = 7;

	IndexBuffer->Unmap(0, nullptr);

	SAFE_DX(ConstantBuffer->Map(0, nullptr, &BufferData));

	DirectX::XMMATRIX WorldMatrix = DirectX::XMMatrixRotationRollPitchYaw(3.14f / 4, 0.0f, 3.14f / 4);
	DirectX::XMMATRIX ViewMatrix = DirectX::XMMatrixLookToLH(DirectX::XMVectorSet(0.0f, 0.0f, -2.5f, 1.0f), DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
	DirectX::XMMATRIX ProjMatrix = DirectX::XMMatrixPerspectiveFovLH(3.14f / 2, 16.0f / 9.0f, 0.01f, 1000.0f);
	DirectX::XMMATRIX WVPMatrix = WorldMatrix * ViewMatrix * ProjMatrix;

	memcpy(BufferData, &WVPMatrix, sizeof(DirectX::XMMATRIX));

	ConstantBuffer->Unmap(0, nullptr);

	D3D12_CPU_DESCRIPTOR_HANDLE ConstantBufferView;
	ConstantBufferView.ptr = CBSRUADescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;

	D3D12_CONSTANT_BUFFER_VIEW_DESC CBVDesc;
	CBVDesc.BufferLocation = ConstantBuffer->GetGPUVirtualAddress();
	CBVDesc.SizeInBytes = 256;

	Device->CreateConstantBufferView(&CBVDesc, ConstantBufferView);

	D3D12_DESCRIPTOR_RANGE DescriptorRanges[2] =
	{
		{ D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }
	};

	D3D12_ROOT_PARAMETER RootParameters[2];
	RootParameters[0] = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &DescriptorRanges[0] }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX };
	RootParameters[1] = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &DescriptorRanges[1] }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL };

	D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc;
	RootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	RootSignatureDesc.NumParameters = 2;
	RootSignatureDesc.NumStaticSamplers = 0;
	RootSignatureDesc.pParameters = RootParameters;
	RootSignatureDesc.pStaticSamplers = nullptr;

	ComPtr<ID3DBlob> RootSignatureBlob;
	ComPtr<ID3DBlob> ErrorBlob;
	SAFE_DX(D3D12SerializeRootSignature(&RootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &RootSignatureBlob, &ErrorBlob));

	ComPtr<ID3D12RootSignature> RootSignature;
	SAFE_DX(Device->CreateRootSignature(0, RootSignatureBlob->GetBufferPointer(), RootSignatureBlob->GetBufferSize(), IID_PPV_ARGS(RootSignature.ReleaseAndGetAddressOf())));

	ComPtr<ID3DBlob> CubeVertexShaderBlob;
	CompileShader(CubeVertexShaderSource, "CubeVertexShader", "VS", "vs_5_0", CubeVertexShaderBlob);

	D3D12_INPUT_ELEMENT_DESC InputElementDesc = 
	{
		"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC GraphicsPipelineStateDesc;
	ZeroMemory(&GraphicsPipelineStateDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	GraphicsPipelineStateDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	GraphicsPipelineStateDesc.DepthStencilState = { .DepthEnable = TRUE, .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL, .DepthFunc = D3D12_COMPARISON_FUNC_LESS };
	GraphicsPipelineStateDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	GraphicsPipelineStateDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	GraphicsPipelineStateDesc.InputLayout.NumElements = 1;
	GraphicsPipelineStateDesc.InputLayout.pInputElementDescs = &InputElementDesc;
	GraphicsPipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	GraphicsPipelineStateDesc.pRootSignature = RootSignature.Get();
	GraphicsPipelineStateDesc.RasterizerState = { .FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_BACK };
	GraphicsPipelineStateDesc.SampleDesc = { 8, 0 };
	GraphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	GraphicsPipelineStateDesc.VS = { CubeVertexShaderBlob->GetBufferPointer(), CubeVertexShaderBlob->GetBufferSize() };

	ComPtr<ID3D12PipelineState> CubeDrawPipeline;
	SAFE_DX(Device->CreateGraphicsPipelineState(&GraphicsPipelineStateDesc, IID_PPV_ARGS(CubeDrawPipeline.ReleaseAndGetAddressOf())));

	ComPtr<ID3DBlob> FSQuadVertexShaderBlob;
	CompileShader(FSQuadVertexShaderSource, "FSQuadVertexShader", "VS", "vs_5_0", FSQuadVertexShaderBlob);

	ComPtr<ID3DBlob> FSQuadPixelShaderBlob;
	CompileShader(FSQuadPixelShaderSource, "FSQuadPixelShader", "PS", "ps_5_0", FSQuadPixelShaderBlob);

	ZeroMemory(&GraphicsPipelineStateDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	GraphicsPipelineStateDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	GraphicsPipelineStateDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	GraphicsPipelineStateDesc.NumRenderTargets = 1;
	GraphicsPipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	GraphicsPipelineStateDesc.pRootSignature = RootSignature.Get();
	GraphicsPipelineStateDesc.PS = { FSQuadPixelShaderBlob->GetBufferPointer(), FSQuadPixelShaderBlob->GetBufferSize() };
	GraphicsPipelineStateDesc.RasterizerState = { .FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_BACK };
	GraphicsPipelineStateDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	GraphicsPipelineStateDesc.SampleDesc = { 1, 0 };
	GraphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	GraphicsPipelineStateDesc.VS = { FSQuadVertexShaderBlob->GetBufferPointer(), FSQuadVertexShaderBlob->GetBufferSize() };

	ComPtr<ID3D12PipelineState> FSQuadDrawPipeline;
	SAFE_DX(Device->CreateGraphicsPipelineState(&GraphicsPipelineStateDesc, IID_PPV_ARGS(FSQuadDrawPipeline.ReleaseAndGetAddressOf())));

	UINT CurrentCommandAllocatorIndex = 0;
	UINT CurrentBackBufferIndex = SwapChain->GetCurrentBackBufferIndex();

	// Set the required callback functions
	glfwSetKeyCallback(window, key_callback);
	glfwShowWindow(window);

	printf("Ready!\n");

	// Main loop
	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(0));

		glfwPollEvents();

		if (glfwWindowShouldClose(window))
			break;

		if (FrameFences[CurrentCommandAllocatorIndex]->GetCompletedValue() != 1)
		{
			SAFE_DX(FrameFences[CurrentCommandAllocatorIndex]->SetEventOnCompletion(1, FrameEvent));
			WaitForSingleObject(FrameEvent, INFINITE);
		}

		SAFE_DX(FrameFences[CurrentCommandAllocatorIndex]->Signal(0));

		SAFE_DX(CommandAllocators[CurrentCommandAllocatorIndex]->Reset());
		SAFE_DX(CommandList->Reset(CommandAllocators[CurrentCommandAllocatorIndex].Get(), nullptr));

		ID3D12DescriptorHeap* ppCB = { CBSRUADescriptorHeap.Get() };
		CommandList->SetDescriptorHeaps(1, &ppCB);
		CommandList->SetGraphicsRootSignature(RootSignature.Get());

		D3D12_RESOURCE_BARRIER ResourceBarrier;
		ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		ResourceBarrier.Transition = { DepthBufferTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE };
		ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		CommandList->ResourceBarrier(1, &ResourceBarrier);

		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthBufferTextureDSV);

		CommandList->ClearDepthStencilView(DepthBufferTextureDSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

		D3D12_VIEWPORT Viewport = { 0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, 1.0f };
		D3D12_RECT ScissorRect = { 0, 0, static_cast<LONG>(windowWidth), static_cast<LONG>(windowHeight) };

		CommandList->RSSetViewports(1, &Viewport);
		CommandList->RSSetScissorRects(1, &ScissorRect);

		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		D3D12_VERTEX_BUFFER_VIEW VertexBufferView;
		VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
		VertexBufferView.SizeInBytes = 8 * sizeof(DirectX::XMFLOAT3);
		VertexBufferView.StrideInBytes = sizeof(DirectX::XMFLOAT3);

		D3D12_INDEX_BUFFER_VIEW IndexBufferView;
		IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
		IndexBufferView.Format = DXGI_FORMAT_R16_UINT;
		IndexBufferView.SizeInBytes = 36 * sizeof(WORD);

		CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
		CommandList->IASetIndexBuffer(&IndexBufferView);
		CommandList->SetPipelineState(CubeDrawPipeline.Get());
		CommandList->SetGraphicsRootDescriptorTable(0, D3D12_GPU_DESCRIPTOR_HANDLE{ CBSRUADescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr });
		CommandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

		ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		ResourceBarrier.Transition = { DepthBufferTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_RESOLVE_SOURCE };
		ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		CommandList->ResourceBarrier(1, &ResourceBarrier);

		ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		ResourceBarrier.Transition = { ResolvedDepthBufferTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST };
		ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		CommandList->ResourceBarrier(1, &ResourceBarrier);

		D3D12_RECT Rect = { 0, 0, static_cast<LONG>(windowWidth), static_cast<LONG>(windowHeight) };
		
		CommandList1->ResolveSubresourceRegion(ResolvedDepthBufferTexture.Get(), 0, 0, 0, DepthBufferTexture.Get(), 0, &Rect, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, D3D12_RESOLVE_MODE_MAX);

		ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		ResourceBarrier.Transition = { ResolvedDepthBufferTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
		ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		CommandList->ResourceBarrier(1, &ResourceBarrier);

		ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		ResourceBarrier.Transition = { BackBufferTextures[CurrentBackBufferIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
		ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		CommandList->ResourceBarrier(1, &ResourceBarrier);

		CommandList->OMSetRenderTargets(1, &BackBufferTexturesRTVs[CurrentBackBufferIndex], FALSE, nullptr);
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		CommandList->SetPipelineState(FSQuadDrawPipeline.Get());
		CommandList->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{ CBSRUADescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr + Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) });
		CommandList->DrawInstanced(4, 1, 0, 0);

		ResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		ResourceBarrier.Transition = { BackBufferTextures[CurrentBackBufferIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT };
		ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		CommandList->ResourceBarrier(1, &ResourceBarrier);

		SAFE_DX(CommandList->Close());

		ID3D12CommandList* ppCommandLists[] = { CommandList.Get() };
		CommandQueue->ExecuteCommandLists(1, ppCommandLists);

		SAFE_DX(SwapChain->Present(1, 0));

		SAFE_DX(CommandQueue->Signal(FrameFences[CurrentCommandAllocatorIndex].Get(), 1));

		CurrentCommandAllocatorIndex = (CurrentCommandAllocatorIndex + 1) % 2;

		CurrentBackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
	}

	CurrentCommandAllocatorIndex = (CurrentCommandAllocatorIndex + 1) % 2;

	if (FrameFences[CurrentCommandAllocatorIndex]->GetCompletedValue() != 1)
	{
		SAFE_DX(FrameFences[CurrentCommandAllocatorIndex]->SetEventOnCompletion(1, FrameEvent));
		WaitForSingleObject(FrameEvent, INFINITE);
	}

	printf("Shutting down...\n");

	glfwDestroyWindow(window);
	glfwTerminate();
	
	return 0;
}