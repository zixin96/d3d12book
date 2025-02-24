﻿#include "DepthComplexityApp.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

DepthComplexityApp::DepthComplexityApp(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
}

DepthComplexityApp::~DepthComplexityApp()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool DepthComplexityApp::Initialize()
{
	if (!D3DApp::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.03f, 4.0f, 0.2f);

	LoadTextures();
	BuildRootSignature();
	BuildDescriptorHeaps();
	BuildShadersAndInputLayout();
	BuildLandGeometry();
	BuildWavesGeometry();
	BuildBoxGeometry();
	BuildMaterials();
	BuildRenderItems();
	BuildFrameResources();
	BuildPSOs();

	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = {mCommandList.Get()};
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void DepthComplexityApp::OnResize()
{
	D3DApp::OnResize();

	// The window resized, so update the aspect ratio and recompute the projection matrix.
	XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
	XMStoreFloat4x4(&mProj, P);
}

void DepthComplexityApp::Update(const GameTimer& gt)
{
	OnKeyboardInput(gt);
	UpdateCamera(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource      = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	AnimateMaterials(gt);
	UpdateObjectCBs(gt);
	UpdateMaterialCBs(gt);
	UpdateMainPassCB(gt);
	UpdateWaves(gt);
}

void DepthComplexityApp::Draw(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr)); // Initial PSO is nullptr to avoid PIX "initial PSO not using" warning

	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
	                                                                       D3D12_RESOURCE_STATE_PRESENT,
	                                                                       D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear the back buffer and depth buffer.
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), (float*)&mMainPassCB.FogColor, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	ID3D12DescriptorHeap* descriptorHeaps[] = {mSrvDescriptorHeap.Get()};
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

	mCommandList->SetPipelineState(mPSOs["opaqueCounter"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	mCommandList->SetPipelineState(mPSOs["alphaTestedCounter"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::AlphaTested]);

	mCommandList->SetPipelineState(mPSOs["transparentCounter"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Transparent]);

	//!? draw the depth complexity of the scene
	mCommandList->SetPipelineState(mPSOs["drawing"].Get());
	for (int i = 0; i < 5; i++) //! Notice that we hardcode 5 here: we have 5 complexity levels defined in the shader
	{
		mCommandList->OMSetStencilRef(i);
		mCommandList->SetGraphicsRoot32BitConstant(4, i, 0);
		mCommandList->IASetVertexBuffers(0, 0, nullptr);
		mCommandList->IASetIndexBuffer(nullptr);
		mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		mCommandList->DrawInstanced(6, 1, 0, 0);
	}

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
	                                                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
	                                                                       D3D12_RESOURCE_STATE_PRESENT));

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = {mCommandList.Get()};
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Swap the back and front buffers
	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SWAP_CHAIN_BUFFER_COUNT;

	// Advance the fence value to mark commands up to this fence point.
	mCurrFrameResource->Fence = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point. 
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void DepthComplexityApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	SetCapture(mhMainWnd);
}

void DepthComplexityApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void DepthComplexityApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		// Update angles based on input to orbit camera around box.
		mTheta += dx;
		mPhi += dy;

		// Restrict the angle mPhi.
		mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		// Make each pixel correspond to 0.2 unit in the scene.
		float dx = 0.2f * static_cast<float>(x - mLastMousePos.x);
		float dy = 0.2f * static_cast<float>(y - mLastMousePos.y);

		// Update the camera radius based on input.
		mRadius += dx - dy;

		// Restrict the radius.
		mRadius = MathHelper::Clamp(mRadius, 5.0f, 150.0f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void DepthComplexityApp::OnKeyboardInput(const GameTimer& gt)
{
}

void DepthComplexityApp::UpdateCamera(const GameTimer& gt)
{
	// Convert Spherical to Cartesian coordinates.
	mEyePos.x = mRadius * sinf(mPhi) * cosf(mTheta);
	mEyePos.z = mRadius * sinf(mPhi) * sinf(mTheta);
	mEyePos.y = mRadius * cosf(mPhi);

	// Build the view matrix.
	XMVECTOR pos    = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);
}

void DepthComplexityApp::AnimateMaterials(const GameTimer& gt)
{
	// Scroll the water material texture coordinates.
	auto waterMat = mMaterials["water"].get();

	float& tu = waterMat->MatTransform(3, 0);
	float& tv = waterMat->MatTransform(3, 1);

	tu += 0.1f * gt.DeltaTime();
	tv += 0.02f * gt.DeltaTime();

	if (tu >= 1.0f)
		tu -= 1.0f;

	if (tv >= 1.0f)
		tv -= 1.0f;

	waterMat->MatTransform(3, 0) = tu;
	waterMat->MatTransform(3, 1) = tv;

	// Material has changed, so need to update cbuffer.
	waterMat->NumFramesDirty = gNumFrameResources;
}

void DepthComplexityApp::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if (e->NumFramesDirty > 0)
		{
			XMMATRIX world        = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void DepthComplexityApp::UpdateMaterialCBs(const GameTimer& gt)
{
	auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
	for (auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0     = mat->FresnelR0;
			matConstants.Roughness     = mat->Roughness;
			XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));

			currMaterialCB->CopyData(mat->MatCBIndex, matConstants);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void DepthComplexityApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);

	XMMATRIX viewProj    = XMMatrixMultiply(view, proj);
	XMMATRIX invView     = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj     = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mMainPassCB.EyePosW             = mEyePos;
	mMainPassCB.RenderTargetSize    = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ               = 1.0f;
	mMainPassCB.FarZ                = 1000.0f;
	mMainPassCB.TotalTime           = gt.TotalTime();
	mMainPassCB.DeltaTime           = gt.DeltaTime();
	mMainPassCB.AmbientLight        = {0.25f, 0.25f, 0.35f, 1.0f};
	mMainPassCB.Lights[0].Direction = {0.57735f, -0.57735f, 0.57735f};
	mMainPassCB.Lights[0].Strength  = {0.9f, 0.9f, 0.8f};
	mMainPassCB.Lights[1].Direction = {-0.57735f, -0.57735f, 0.57735f};
	mMainPassCB.Lights[1].Strength  = {0.3f, 0.3f, 0.3f};
	mMainPassCB.Lights[2].Direction = {0.0f, -0.707f, -0.707f};
	mMainPassCB.Lights[2].Strength  = {0.15f, 0.15f, 0.15f};

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void DepthComplexityApp::UpdateWaves(const GameTimer& gt)
{
	// Every quarter second, generate a random wave.
	static float t_base = 0.0f;
	if ((mTimer.TotalTime() - t_base) >= 0.25f)
	{
		t_base += 0.25f;

		int i = MathHelper::Rand(4, mWaves->RowCount() - 5);
		int j = MathHelper::Rand(4, mWaves->ColumnCount() - 5);

		float r = MathHelper::RandF(0.2f, 0.5f);

		mWaves->Disturb(i, j, r);
	}

	// Update the wave simulation.
	mWaves->Update(gt.DeltaTime());

	// Update the wave vertex buffer with the new solution.
	auto currWavesVB = mCurrFrameResource->WavesVB.get();
	for (int i = 0; i < mWaves->VertexCount(); ++i)
	{
		Vertex v;

		v.Pos    = mWaves->Position(i);
		v.Normal = mWaves->Normal(i);

		// Derive tex-coords from position by 
		// mapping [-w/2,w/2] --> [0,1]
		v.TexC.x = 0.5f + v.Pos.x / mWaves->Width();
		v.TexC.y = 0.5f - v.Pos.z / mWaves->Depth();

		currWavesVB->CopyData(i, v);
	}

	// Set the dynamic VB of the wave renderitem to the current frame VB.
	mWavesRitem->Geo->VertexBufferGPU = currWavesVB->Resource();
}

void DepthComplexityApp::LoadTextures()
{
	auto grassTex      = std::make_unique<Texture>();
	grassTex->Name     = "grassTex";
	grassTex->Filename = L"../../Textures/grass.dds";

	auto waterTex      = std::make_unique<Texture>();
	waterTex->Name     = "waterTex";
	waterTex->Filename = L"../../Textures/water1.dds";

	auto fenceTex      = std::make_unique<Texture>();
	fenceTex->Name     = "fenceTex";
	fenceTex->Filename = L"../../Textures/WireFence.dds";
	//fenceTex->Filename = L"../../Textures/WoodCrate02.dds";

	mTextures[grassTex->Name] = std::move(grassTex);
	mTextures[waterTex->Name] = std::move(waterTex);
	mTextures[fenceTex->Name] = std::move(fenceTex);

	for (auto& tex : mTextures)
	{
		tex.second->Resource = d3dUtil::CreateTexture(md3dDevice.Get(),
		                                              mCommandList.Get(),
		                                              tex.second->Filename.c_str(),
		                                              tex.second->UploadHeap);
	}
}

void DepthComplexityApp::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[1].InitAsConstantBufferView(0);
	slotRootParameter[2].InitAsConstantBufferView(1);
	slotRootParameter[3].InitAsConstantBufferView(2);
	slotRootParameter[4].InitAsConstants(1, 3);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
	                                        (UINT)staticSamplers.size(), staticSamplers.data(),
	                                        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob         = nullptr;
	HRESULT          hr                = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                                                 serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		              0,
		              serializedRootSig->GetBufferPointer(),
		              serializedRootSig->GetBufferSize(),
		              IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void DepthComplexityApp::BuildDescriptorHeaps()
{
	//
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors             = 3;
	srvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	//
	// Fill out the heap with actual descriptors.
	//
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	auto grassTex = mTextures["grassTex"]->Resource;
	auto waterTex = mTextures["waterTex"]->Resource;
	auto fenceTex = mTextures["fenceTex"]->Resource;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format                          = grassTex->GetDesc().Format;
	srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip       = 0;
	srvDesc.Texture2D.MipLevels             = -1;
	md3dDevice->CreateShaderResourceView(grassTex.Get(), &srvDesc, hDescriptor);

	// next descriptor
	hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

	srvDesc.Format = waterTex->GetDesc().Format;
	md3dDevice->CreateShaderResourceView(waterTex.Get(), &srvDesc, hDescriptor);

	// next descriptor
	hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);

	srvDesc.Format = fenceTex->GetDesc().Format;
	md3dDevice->CreateShaderResourceView(fenceTex.Get(), &srvDesc, hDescriptor);
}

void DepthComplexityApp::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO defines[] =
	{
		"FOG", "1", // some scenes may not want to use FOG; therefore we make fog optional by requiring FOG to be defined when compiling the shader
		NULL, NULL  // The last structure in the array serves as a terminator and must have all members set to NULL.
	};

	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"FOG", "1",
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	mShaders["standardVS"]    = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["opaquePS"]      = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", defines, "PS", "ps_5_0");
	mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", alphaTestDefines, "PS", "ps_5_0"); // alpha tested shaders are used when drawing objects where pixels are either completely opaque or completely transparent

	mShaders["colorQuadVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VSColorQuad", "vs_5_0");
	mShaders["colorQuadPS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PSColorQuad", "ps_5_0");

	mInputLayout =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};
}

void DepthComplexityApp::BuildLandGeometry()
{
	GeometryGenerator           geoGen;
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(160.0f, 160.0f, 50, 50);

	//
	// Extract the vertex elements we are interested and apply the height function to
	// each vertex.  In addition, color the vertices based on their height so we have
	// sandy looking beaches, grassy low hills, and snow mountain peaks.
	//

	std::vector<Vertex> vertices(grid.Vertices.size());
	for (size_t i = 0; i < grid.Vertices.size(); ++i)
	{
		auto& p            = grid.Vertices[i].Position;
		vertices[i].Pos    = p;
		vertices[i].Pos.y  = GetHillsHeight(p.x, p.z);
		vertices[i].Normal = GetHillsNormal(p.x, p.z);
		vertices[i].TexC   = grid.Vertices[i].TexC;
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	std::vector<std::uint16_t> indices    = grid.GetIndices16();
	const UINT                 ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo  = std::make_unique<MeshGeometry>();
	geo->Name = "landGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
	                                                    mCommandList.Get(),
	                                                    vertices.data(),
	                                                    vbByteSize,
	                                                    geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
	                                                   mCommandList.Get(),
	                                                   indices.data(),
	                                                   ibByteSize,
	                                                   geo->IndexBufferUploader);

	geo->VertexByteStride     = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount         = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["grid"] = submesh;

	mGeometries["landGeo"] = std::move(geo);
}

void DepthComplexityApp::BuildWavesGeometry()
{
	std::vector<std::uint16_t> indices(3 * mWaves->TriangleCount()); // 3 indices per face
	assert(mWaves->VertexCount() < 0x0000ffff);

	// Iterate over each quad.
	int m = mWaves->RowCount();
	int n = mWaves->ColumnCount();
	int k = 0;
	for (int i = 0; i < m - 1; ++i)
	{
		for (int j = 0; j < n - 1; ++j)
		{
			indices[k]     = i * n + j;
			indices[k + 1] = i * n + j + 1;
			indices[k + 2] = (i + 1) * n + j;

			indices[k + 3] = (i + 1) * n + j;
			indices[k + 4] = i * n + j + 1;
			indices[k + 5] = (i + 1) * n + j + 1;

			k += 6; // next quad
		}
	}

	UINT vbByteSize = mWaves->VertexCount() * sizeof(Vertex);
	UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo  = std::make_unique<MeshGeometry>();
	geo->Name = "waterGeo";

	// Set dynamically.
	geo->VertexBufferCPU = nullptr;
	geo->VertexBufferGPU = nullptr;

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
	                                                   mCommandList.Get(),
	                                                   indices.data(),
	                                                   ibByteSize,
	                                                   geo->IndexBufferUploader);

	geo->VertexByteStride     = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount         = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["grid"] = submesh;

	mGeometries["waterGeo"] = std::move(geo);
}

void DepthComplexityApp::BuildBoxGeometry()
{
	GeometryGenerator           geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(8.0f, 8.0f, 8.0f, 3);

	std::vector<Vertex> vertices(box.Vertices.size());
	for (size_t i = 0; i < box.Vertices.size(); ++i)
	{
		auto& p            = box.Vertices[i].Position;
		vertices[i].Pos    = p;
		vertices[i].Normal = box.Vertices[i].Normal;
		vertices[i].TexC   = box.Vertices[i].TexC;
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	std::vector<std::uint16_t> indices    = box.GetIndices16();
	const UINT                 ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo  = std::make_unique<MeshGeometry>();
	geo->Name = "boxGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
	                                                    mCommandList.Get(),
	                                                    vertices.data(),
	                                                    vbByteSize,
	                                                    geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
	                                                   mCommandList.Get(),
	                                                   indices.data(),
	                                                   ibByteSize,
	                                                   geo->IndexBufferUploader);

	geo->VertexByteStride     = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount         = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["box"] = submesh;

	mGeometries["boxGeo"] = std::move(geo);
}

void DepthComplexityApp::BuildPSOs()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
	ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout    = {mInputLayout.data(), (UINT)mInputLayout.size()};
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS             =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // default: AlphaToCoverageEnable = false, IndependentBlendEnable = false
	opaquePsoDesc.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask            = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets      = 1;
	opaquePsoDesc.RTVFormats[0]         = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count      = 1;
	opaquePsoDesc.SampleDesc.Quality    = 0;
	opaquePsoDesc.DSVFormat             = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	//
	// PSO for transparent objects
	//

	// start from non-blended pso
	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = opaquePsoDesc;

	// specify how blending is done for a render target 
	D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
	transparencyBlendDesc.BlendEnable   = true;  // either use blend or logic op. Cannot use both
	transparencyBlendDesc.LogicOpEnable = false; // either use blend or logic op. Cannot use both

	transparencyBlendDesc.SrcBlend  = D3D12_BLEND_SRC_ALPHA;
	transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparencyBlendDesc.BlendOp   = D3D12_BLEND_OP_ADD;

	transparencyBlendDesc.SrcBlendAlpha         = D3D12_BLEND_ONE;
	transparencyBlendDesc.DestBlendAlpha        = D3D12_BLEND_ZERO;
	transparencyBlendDesc.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.LogicOp               = D3D12_LOGIC_OP_NOOP;
	transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc; // since IndependentBlendEnable = false, all the render targets use RenderTarget[0] for blending
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));

	//
	// PSO for alpha tested objects
	//

	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedPsoDesc = opaquePsoDesc;
	alphaTestedPsoDesc.PS                                 =
	{
		reinterpret_cast<BYTE*>(mShaders["alphaTestedPS"]->GetBufferPointer()),
		mShaders["alphaTestedPS"]->GetBufferSize()
	};
	alphaTestedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // disable back face culling for alpha tested objects (because we can now see through the objects with alpha-enabled textures)
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDesc, IID_PPV_ARGS(&mPSOs["alphaTested"])));

	//!? Create DEPTH_STENCIL_DESC for accumulating depth complexity
	D3D12_DEPTH_STENCIL_DESC counterDepthStencilDesc;
	//!? Set DepthEnable = false to obtain depth complexity: indicates how many triangles overlapped each pixel, regardless of sorting or z process
	counterDepthStencilDesc.DepthEnable = false;
	//!? Set DepthEnable = true to obtain overdraw: indicates how many pixels were shaded and written to the framebuffer after passing the depth test
	// stencilMarker.DepthEnable      = true;
	counterDepthStencilDesc.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
	counterDepthStencilDesc.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
	counterDepthStencilDesc.StencilEnable    = TRUE;
	counterDepthStencilDesc.StencilReadMask  = 0xff; //! Note: this is 0xff, not true or false. Be mindful of copy-paste error!
	counterDepthStencilDesc.StencilWriteMask = 0xff;

	counterDepthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	counterDepthStencilDesc.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	counterDepthStencilDesc.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_INCR;
	counterDepthStencilDesc.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;

	counterDepthStencilDesc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	counterDepthStencilDesc.BackFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	counterDepthStencilDesc.BackFace.StencilPassOp      = D3D12_STENCIL_OP_INCR;
	counterDepthStencilDesc.BackFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;

	//!? create verions of PSO that populate stencil buffer with depth compelxity
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueCounter = opaquePsoDesc;
	opaqueCounter.DepthStencilState                  = counterDepthStencilDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueCounter, IID_PPV_ARGS(&mPSOs["opaqueCounter"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentCounter = transparentPsoDesc;
	transparentCounter.DepthStencilState                  = counterDepthStencilDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentCounter, IID_PPV_ARGS(&mPSOs["transparentCounter"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedCounter = alphaTestedPsoDesc;
	alphaTestedCounter.DepthStencilState                  = counterDepthStencilDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedCounter, IID_PPV_ARGS(&mPSOs["alphaTestedCounter"])));

	//!? Create DEPTH_STENCIL_DESC for drawing depth complexity
	D3D12_DEPTH_STENCIL_DESC drawingDepthStencilDesc;
	drawingDepthStencilDesc.DepthEnable      = false;
	drawingDepthStencilDesc.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
	drawingDepthStencilDesc.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
	drawingDepthStencilDesc.StencilEnable    = TRUE;
	drawingDepthStencilDesc.StencilReadMask  = 0xff; //! Note: this is 0xff, not true or false. Be mindful of copy-paste error!
	drawingDepthStencilDesc.StencilWriteMask = 0xff;

	drawingDepthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	drawingDepthStencilDesc.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	drawingDepthStencilDesc.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	drawingDepthStencilDesc.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_EQUAL;

	drawingDepthStencilDesc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	drawingDepthStencilDesc.BackFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	drawingDepthStencilDesc.BackFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	drawingDepthStencilDesc.BackFace.StencilFunc        = D3D12_COMPARISON_FUNC_EQUAL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC drawingPsoDesc = opaquePsoDesc;
	drawingPsoDesc.VS                                 =
	{
		reinterpret_cast<BYTE*>(mShaders["colorQuadVS"]->GetBufferPointer()),
		mShaders["colorQuadVS"]->GetBufferSize()
	};
	drawingPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["colorQuadPS"]->GetBufferPointer()),
		mShaders["colorQuadPS"]->GetBufferSize()
	};
	drawingPsoDesc.DepthStencilState = drawingDepthStencilDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawingPsoDesc, IID_PPV_ARGS(&mPSOs["drawing"])));

	// //!?  PSO for drawing depth complexity:
	// D3D12_GRAPHICS_PIPELINE_STATE_DESC drawDepthComplexPsoDesc = opaquePsoDesc;
	//
	// // modify PSO's DepthStencilState
	// D3D12_DEPTH_STENCIL_DESC drawDSS;
	// drawDSS.DepthEnable    = false;
	// drawDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	// drawDSS.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
	//
	// drawDSS.StencilEnable    = true;
	// drawDSS.StencilReadMask  = 0xff;
	// drawDSS.StencilWriteMask = 0xff;
	//
	// // how the stencil buffer works for front facing triangles: 
	// drawDSS.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
	// drawDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	// drawDSS.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
	// drawDSS.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_EQUAL;
	//
	// // how the stencil buffer works for back facing triangles: 
	// // If we are not rendering backfacing polygons, these settings do not matter.
	// drawDSS.BackFace.StencilFailOp            = D3D12_STENCIL_OP_KEEP;
	// drawDSS.BackFace.StencilDepthFailOp       = D3D12_STENCIL_OP_KEEP;
	// drawDSS.BackFace.StencilPassOp            = D3D12_STENCIL_OP_KEEP;
	// drawDSS.BackFace.StencilFunc              = D3D12_COMPARISON_FUNC_EQUAL;
	// drawDepthComplexPsoDesc.DepthStencilState = drawDSS;
	//
	// ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawDepthComplexPsoDesc, IID_PPV_ARGS(&mPSOs["drawStencil"])));
}

void DepthComplexityApp::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
		                                                          1,
		                                                          (UINT)mAllRitems.size(),
		                                                          (UINT)mMaterials.size(),
		                                                          mWaves->VertexCount()));
	}
}

void DepthComplexityApp::BuildMaterials()
{
	auto grass                 = std::make_unique<Material>();
	grass->Name                = "grass";
	grass->MatCBIndex          = 0;
	grass->DiffuseSrvHeapIndex = 0;
	grass->DiffuseAlbedo       = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	grass->FresnelR0           = XMFLOAT3(0.01f, 0.01f, 0.01f);
	grass->Roughness           = 0.125f;

	// This is not a good water material definition, but we do not have all the rendering
	// tools we need (transparency, environment reflection), so we fake it for now.
	auto water                 = std::make_unique<Material>();
	water->Name                = "water";
	water->MatCBIndex          = 1;
	water->DiffuseSrvHeapIndex = 1;
	water->DiffuseAlbedo       = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.5f); //! Note: our texture has alpha = 1, so we modulate the alpha value of water texture by providing a diffuseAlbedo with alpha = 0.5 (to make it transparent)
	water->FresnelR0           = XMFLOAT3(0.1f, 0.1f, 0.1f);
	water->Roughness           = 0.0f;

	auto wirefence                 = std::make_unique<Material>();
	wirefence->Name                = "wirefence";
	wirefence->MatCBIndex          = 2;
	wirefence->DiffuseSrvHeapIndex = 2;
	wirefence->DiffuseAlbedo       = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	wirefence->FresnelR0           = XMFLOAT3(0.1f, 0.1f, 0.1f);
	wirefence->Roughness           = 0.25f;

	mMaterials["grass"]     = std::move(grass);
	mMaterials["water"]     = std::move(water);
	mMaterials["wirefence"] = std::move(wirefence);
}

void DepthComplexityApp::BuildRenderItems()
{
	auto wavesRitem   = std::make_unique<RenderItem>();
	wavesRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&wavesRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	wavesRitem->ObjCBIndex         = 0;
	wavesRitem->Mat                = mMaterials["water"].get();
	wavesRitem->Geo                = mGeometries["waterGeo"].get();
	wavesRitem->PrimitiveType      = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wavesRitem->IndexCount         = wavesRitem->Geo->DrawArgs["grid"].IndexCount;
	wavesRitem->StartIndexLocation = wavesRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	wavesRitem->BaseVertexLocation = wavesRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

	mWavesRitem = wavesRitem.get();

	mRitemLayer[(int)RenderLayer::Transparent].push_back(wavesRitem.get());

	auto gridRitem   = std::make_unique<RenderItem>();
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	gridRitem->ObjCBIndex         = 1;
	gridRitem->Mat                = mMaterials["grass"].get();
	gridRitem->Geo                = mGeometries["landGeo"].get();
	gridRitem->PrimitiveType      = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount         = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

	mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());

	auto boxRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxRitem->World, XMMatrixTranslation(3.0f, 2.0f, -9.0f));
	boxRitem->ObjCBIndex         = 2;
	boxRitem->Mat                = mMaterials["wirefence"].get();
	boxRitem->Geo                = mGeometries["boxGeo"].get();
	boxRitem->PrimitiveType      = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount         = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;

	mRitemLayer[(int)RenderLayer::AlphaTested].push_back(boxRitem.get());

	mAllRitems.push_back(std::move(wavesRitem));
	mAllRitems.push_back(std::move(gridRitem));
	mAllRitems.push_back(std::move(boxRitem));
}


void DepthComplexityApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	auto matCB    = mCurrFrameResource->MaterialCB->Resource();

	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		tex.Offset(ri->Mat->DiffuseSrvHeapIndex, mCbvSrvUavDescriptorSize);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

		cmdList->SetGraphicsRootDescriptorTable(0, tex);
		cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
		cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> DepthComplexityApp::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
	                                            0,                                // shaderRegister
	                                            D3D12_FILTER_MIN_MAG_MIP_POINT,   // filter
	                                            D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
	                                            D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
	                                            D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
	                                             1,                                 // shaderRegister
	                                             D3D12_FILTER_MIN_MAG_MIP_POINT,    // filter
	                                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
	                                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
	                                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
	                                             2,                                // shaderRegister
	                                             D3D12_FILTER_MIN_MAG_MIP_LINEAR,  // filter
	                                             D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
	                                             D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
	                                             D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
	                                              3,                                 // shaderRegister
	                                              D3D12_FILTER_MIN_MAG_MIP_LINEAR,   // filter
	                                              D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
	                                              D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
	                                              D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
	                                                  4,                               // shaderRegister
	                                                  D3D12_FILTER_ANISOTROPIC,        // filter
	                                                  D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
	                                                  D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
	                                                  D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressW
	                                                  0.0f,                            // mipLODBias
	                                                  8);                              // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
	                                                   5,                                // shaderRegister
	                                                   D3D12_FILTER_ANISOTROPIC,         // filter
	                                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
	                                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
	                                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressW
	                                                   0.0f,                             // mipLODBias
	                                                   8);                               // maxAnisotropy

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp
	};
}

float DepthComplexityApp::GetHillsHeight(float x, float z) const
{
	return 0.3f * (z * sinf(0.1f * x) + x * cosf(0.1f * z));
}

XMFLOAT3 DepthComplexityApp::GetHillsNormal(float x, float z) const
{
	// n = (-df/dx, 1, -df/dz)
	XMFLOAT3 n(
	           -0.03f * z * cosf(0.1f * x) - 0.3f * cosf(0.1f * z),
	           1.0f,
	           -0.3f * sinf(0.1f * x) + 0.03f * x * sinf(0.1f * z));

	XMVECTOR unitNormal = XMVector3Normalize(XMLoadFloat3(&n));
	XMStoreFloat3(&n, unitNormal);

	return n;
}
