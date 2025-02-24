#pragma once

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/Camera.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;


struct RenderItem
{
	RenderItem()                      = default;
	RenderItem(const RenderItem& rhs) = delete;

	XMFLOAT4X4 World        = MathHelper::Identity4x4();
	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	int NumFramesDirty = gNumFrameResources;

	UINT ObjCBIndex = -1;

	Material*     Mat = nullptr;
	MeshGeometry* Geo = nullptr;

	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
	UINT IndexCount         = 0;
	UINT StartIndexLocation = 0;
	int  BaseVertexLocation = 0;
};

class CameraAndDynamicIndexingApp : public D3DApp
{
public:
	CameraAndDynamicIndexingApp(HINSTANCE hInstance);
	CameraAndDynamicIndexingApp(const CameraAndDynamicIndexingApp& rhs)            = delete;
	CameraAndDynamicIndexingApp& operator=(const CameraAndDynamicIndexingApp& rhs) = delete;
	~CameraAndDynamicIndexingApp() override;

	bool Initialize() override;

private:
	void OnResize() override;
	void Update(const GameTimer& gt) override;
	void Draw(const GameTimer& gt) override;

	void OnMouseDown(WPARAM btnState, int x, int y) override;
	void OnMouseUp(WPARAM btnState, int x, int y) override;
	void OnMouseMove(WPARAM btnState, int x, int y) override;

	void OnKeyboardInput(const GameTimer& gt);
	void AnimateMaterials(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMaterialBuffer(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);

	void LoadTextures();
	void BuildRootSignature();
	void BuildDescriptorHeaps();
	void BuildShadersAndInputLayout();
	void BuildShapeGeometry();
	void BuildPSOs();
	void BuildFrameResources();
	void BuildMaterials();
	void BuildRenderItems();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

private:
	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource*                              mCurrFrameResource      = nullptr;
	int                                         mCurrFrameResourceIndex = 0;

	ComPtr<ID3D12RootSignature>  mRootSignature     = nullptr;
	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>>     mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>>      mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>>              mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>>   mPSOs;

	std::vector<D3D12_INPUT_ELEMENT_DESC>    mInputLayout;
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;
	std::vector<RenderItem*>                 mOpaqueRitems;
	PassConstants                            mMainPassCB;
	Camera                                   mCamera;
	POINT                                    mLastMousePos;
};
