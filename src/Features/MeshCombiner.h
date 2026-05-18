#pragma once

#include "Feature.h"

struct MeshCombiner : Feature
{
	virtual inline std::string GetName() override { return "Mesh Combiner"; }
	virtual inline std::string GetShortName() override { return "MeshCombiner"; }
	virtual inline std::string_view GetShaderDefineName() override { return "MESH_COMBINER"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Merges static geometry sharing the same material into combined draw calls, reducing CPU overhead.",
			{ "Automatic grouping by vertex format and material",
				"World-space vertex merging via engine BSTriShape factories",
				"Combined meshes render through the standard engine pipeline",
				"Per-frame statistics and debug visualization" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type) override { return false; }
	virtual bool SupportsVR() override { return false; }

	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void ClearShaderCache() override;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	struct Settings
	{
		bool Enable = false;
		bool HideOriginals = false;
		uint32_t MinGroupSize = 2;
	};
	Settings settings;

	struct CombineKey
	{
		uint64_t vertexDesc = 0;
		RE::BSShaderMaterial* material = nullptr;

		bool operator==(const CombineKey& other) const
		{
			return vertexDesc == other.vertexDesc && material == other.material;
		}
	};

	struct CombineKeyHash
	{
		size_t operator()(const CombineKey& key) const
		{
			size_t h = std::hash<uint64_t>{}(key.vertexDesc);
			h ^= std::hash<const void*>{}(key.material) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};

	struct SourceMesh
	{
		RE::BSGeometry* geometry = nullptr;
		RE::BSGraphics::TriShape* rendererData = nullptr;
		uint16_t vertexCount = 0;
		uint16_t triangleCount = 0;
	};

	struct ActiveCombine
	{
		RE::NiPointer<RE::BSTriShape> triShape;
		RE::NiPointer<RE::NiNode> parent;
		eastl::vector<RE::BSGeometry*> sourceGeometries;
	};

	eastl::vector<ActiveCombine> activeCombines;

	RE::FormID lastPlayerCellID = 0;
	bool combinesDirty = true;

	void RebuildCombines();
	void CollectGeometry(RE::TESObjectCELL* cell,
		eastl::hash_map<CombineKey, eastl::vector<SourceMesh>, CombineKeyHash>& groups);
	RE::NiPointer<RE::BSTriShape> BuildCombinedMesh(
		const CombineKey& key,
		const eastl::vector<SourceMesh>& sources,
		uint32_t startIndex,
		RE::NiNode* parent,
		uint32_t& outSourcesMerged);
	void DestroyCombines();
	void PruneDetachedCombines();
	void SetOriginalVisibility(bool visible);

	uint32_t stat_totalCombines = 0;
	uint32_t stat_totalSourceMeshes = 0;
	uint32_t stat_totalVerticesCombined = 0;
	uint32_t stat_groupsFound = 0;
};
