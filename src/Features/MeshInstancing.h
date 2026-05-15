#pragma once

#include "Buffer.h"

struct MeshInstancing : Feature
{
	virtual inline std::string GetName() override { return "Mesh Instancing"; }
	virtual inline std::string GetShortName() override { return "MeshInstancing"; }
	virtual inline std::string_view GetShaderDefineName() override { return "MESH_INSTANCING"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Batches identical static meshes into instanced draw calls, reducing CPU overhead from repeated geometry.",
			{ "Automatic detection of repeated static meshes",
				"Per-instance world transforms via structured buffer",
				"Configurable minimum instance count threshold",
				"Per-frame statistics for instanced vs individual draws" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;
	virtual bool SupportsVR() override { return true; }

	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void DataLoaded() override;
	virtual void ClearShaderCache() override;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	// ---- Settings ----

	struct Settings
	{
		bool Enabled = true;
		bool DebugSingleInstance = false;
		uint32_t MinInstanceCount = 3;
		uint32_t MaxInstancesPerGroup = 512;
		uint32_t MaxTotalInstances = 16384;
	};
	Settings settings;

	// ---- GPU data ----

	// Per-instance transform uploaded to structured buffer.
	// Each matrix is stored as 3 x float4 rows matching HLSL row_major float3x4.
	struct alignas(16) InstanceTransform
	{
		float4 World[3];
		float4 PreviousWorld[3];
	};
	STATIC_ASSERT_ALIGNAS_16(InstanceTransform);

	static constexpr uint32_t InstanceBufferSRVSlot = 30;

	eastl::unique_ptr<StructuredBuffer> instanceBuffer;

	// ---- Instance group registry ----

	// Key: the GPU-side TriShape pointer identifies meshes sharing the same VB/IB.
	// All placed instances of the same TESObjectSTAT share one TriShape.
	struct MeshKey
	{
		const void* triShape = nullptr;

		bool operator==(const MeshKey& other) const { return triShape == other.triShape; }
	};

	struct MeshKeyHash
	{
		size_t operator()(const MeshKey& key) const
		{
			return std::hash<const void*>{}(key.triShape);
		}
	};

	struct InstanceGroup
	{
		eastl::vector<RE::BSGeometry*> geometries;
		uint32_t bufferOffset = 0;
		bool drawnThisFrame = false;
	};

	eastl::hash_map<MeshKey, InstanceGroup, MeshKeyHash> instanceGroups;

	// Reverse lookup: geometry node → its group key (for O(1) render-time checks)
	eastl::hash_map<const RE::BSGeometry*, MeshKey> geometryToGroup;

	// ---- Cell tracking ----

	RE::FormID lastPlayerCellID = 0;
	bool groupsDirty = true;

	bool RebuildInstanceGroups();
	void ScanCell(RE::TESObjectCELL* cell);
	void UploadInstanceTransforms();

	// ---- Render-time ----

	// Called from the DrawIndexed hook. Returns true if the draw was handled
	// (either upgraded to instanced or suppressed as a duplicate).
	bool HandleDraw(RE::BSGeometry* geometry, uint32_t indexCount, uint32_t startIndex, int32_t baseVertex);

	// ---- Per-frame stats ----

	uint32_t stat_instancedDrawCalls = 0;
	uint32_t stat_suppressedDrawCalls = 0;
	uint32_t stat_totalInstancesRendered = 0;
	uint32_t stat_totalGroups = 0;

	// ---- Hooks ----

	struct Hooks
	{
		// BSLightingShader::SetupGeometry — tracks the current geometry being rendered
		// so the DrawIndexed hook knows which mesh is about to draw.
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ID3D11DeviceContext::DrawIndexed vtable hook — replaces individual draws
		// with DrawIndexedInstanced or suppresses duplicates.
		struct ID3D11DeviceContext_DrawIndexed
		{
			static void STDMETHODCALLTYPE thunk(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};

	// Set by SetupGeometry, read by DrawIndexed. Single-threaded rendering.
	RE::BSGeometry* currentLightingGeometry = nullptr;
	bool inLightingShader = false;
};
