#include "MeshCombiner.h"

#include "Globals.h"
#include "State.h"

void MeshCombiner::SetupResources()
{
}

void MeshCombiner::Reset()
{
}

void MeshCombiner::ClearShaderCache()
{
}

// ---- Core lifecycle ----

void MeshCombiner::Prepass()
{
	if (!settings.Enable)
		return;

	if (!REL::Module::IsAE()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			logger::warn("[MeshCombiner] AE-only during development; SE/VR pending");
		}
		return;
	}

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	auto* cell = player->GetParentCell();
	auto cellID = cell ? cell->GetFormID() : 0;
	if (cellID != lastPlayerCellID) {
		lastPlayerCellID = cellID;
		combinesDirty = true;
	}

	PruneDetachedCombines();

	if (combinesDirty) {
		DestroyCombines();
		RebuildCombines();
		combinesDirty = false;
	}
}

void MeshCombiner::RebuildCombines()
{
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	auto* currentCell = player->GetParentCell();
	if (!currentCell)
		return;

	eastl::hash_map<CombineKey, eastl::vector<SourceMesh>, CombineKeyHash> groups;

	if (currentCell->IsInteriorCell()) {
		return;
	}

	if (auto* gridCells = tes->gridCells) {
		auto gridLength = gridCells->length;
		for (uint32_t x = 0; x < gridLength; x++) {
			for (uint32_t y = 0; y < gridLength; y++) {
				if (auto* cell = gridCells->GetCell(x, y))
					CollectGeometry(cell, groups);
			}
		}
	}

	stat_groupsFound = static_cast<uint32_t>(groups.size());
	stat_totalCombines = 0;
	stat_totalSourceMeshes = 0;
	stat_totalVerticesCombined = 0;

	for (auto& [key, sources] : groups) {
		if (sources.size() < settings.MinGroupSize)
			continue;

		// Find a cell-level parent by walking up past the BSFadeNode (ref root).
		// Cell-level nodes have identity rotation and unit scale, so our
		// world-transformed vertices won't be double-rotated by the parent.
		RE::NiNode* parent = nullptr;
		for (auto& src : sources) {
			if (!src.geometry)
				continue;
			auto* node = src.geometry->parent;
			while (node) {
				if (netimmerse_cast<RE::BSFadeNode*>(node) && node->parent) {
					parent = node->parent;
					break;
				}
				node = node->parent;
			}
			if (parent)
				break;
		}
		if (!parent)
			continue;

		uint32_t cursor = 0;
		while (cursor < sources.size()) {
			uint32_t sourcesMerged = 0;
			auto combined = BuildCombinedMesh(key, sources, cursor, parent, sourcesMerged);
			if (!combined || sourcesMerged == 0) {
				// Source at `cursor` couldn't be merged (e.g. exceeds 65535 verts on its own).
				// Skip it and continue so we don't infinite-loop or stop the whole group.
				cursor++;
				continue;
			}

			ActiveCombine ac;
			ac.triShape = combined;
			ac.parent = RE::NiPointer<RE::NiNode>(parent);
			for (uint32_t i = 0; i < sourcesMerged; i++)
				ac.sourceGeometries.push_back(sources[cursor + i].geometry);

			activeCombines.push_back(std::move(ac));
			stat_totalCombines++;
			stat_totalSourceMeshes += sourcesMerged;
			cursor += sourcesMerged;
		}
	}

	if (settings.HideOriginals)
		SetOriginalVisibility(false);

	logger::info("[MeshCombiner] Built {} combines from {} sources ({} groups, {} total verts)",
		stat_totalCombines, stat_totalSourceMeshes, stat_groupsFound, stat_totalVerticesCombined);
}

void MeshCombiner::CollectGeometry(RE::TESObjectCELL* cell,
	eastl::hash_map<CombineKey, eastl::vector<SourceMesh>, CombineKeyHash>& groups)
{
	if (!cell)
		return;

	cell->ForEachReference([&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
		if (!ref || ref->IsDisabled() || ref->IsDeleted())
			return RE::BSContainer::ForEachResult::kContinue;

		auto* baseObj = ref->GetBaseObject();
		if (!baseObj || baseObj->GetFormType() != RE::FormType::Static)
			return RE::BSContainer::ForEachResult::kContinue;

		auto* node3D = ref->Get3D();
		if (!node3D)
			return RE::BSContainer::ForEachResult::kContinue;

		RE::BSVisit::TraverseScenegraphGeometries(node3D, [&](RE::BSGeometry* geometry) -> RE::BSVisit::BSVisitControl {
			if (!geometry)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto& runtimeData = geometry->GetGeometryRuntimeData();

			if (runtimeData.skinInstance)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto* rd = runtimeData.rendererData;
			if (!rd || !rd->rawVertexData || !rd->rawIndexData)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto* lightingProp = netimmerse_cast<RE::BSLightingShaderProperty*>(runtimeData.shaderProperty.get());
			if (!lightingProp || !lightingProp->material)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto* triShape = netimmerse_cast<RE::BSTriShape*>(geometry);
			if (!triShape)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto& triData = triShape->GetTrishapeRuntimeData();
			if (triData.vertexCount == 0 || triData.triangleCount == 0)
				return RE::BSVisit::BSVisitControl::kContinue;

			uint64_t vd = 0;
			std::memcpy(&vd, &runtimeData.vertexDesc, sizeof(uint64_t));

			CombineKey key;
			key.vertexDesc = vd;
			key.material = lightingProp->material;

			SourceMesh sm;
			sm.geometry = geometry;
			sm.rendererData = rd;
			sm.vertexCount = triData.vertexCount;
			sm.triangleCount = triData.triangleCount;

			groups[key].push_back(sm);

			return RE::BSVisit::BSVisitControl::kContinue;
		});

		return RE::BSContainer::ForEachResult::kContinue;
	});
}

RE::NiPointer<RE::BSTriShape> MeshCombiner::BuildCombinedMesh(
	const CombineKey& key,
	const eastl::vector<SourceMesh>& sources,
	uint32_t startIndex,
	RE::NiNode* parent,
	uint32_t& outSourcesMerged)
{
	outSourcesMerged = 0;
	if (startIndex >= sources.size())
		return nullptr;
	auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto* memmgr = RE::MemoryManager::GetSingleton();
	if (!renderer || !memmgr)
		return nullptr;

	auto& vertexDesc = sources[startIndex].geometry->GetGeometryRuntimeData().vertexDesc;
	const uint32_t vertexStride = vertexDesc.GetSize();
	if (vertexStride == 0 || vertexStride > 128)
		return nullptr;

	// Tangent frame attribute offsets — same for every source in the group (same vertexDesc).
	// Normal+tangent are 4 SNORM8 bytes each; the 4th component of position/normal/tangent
	// stores bitangent.x/y/z respectively, so we must transform N, T, and B together.
	const bool hasNormal = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
	const bool hasTangent = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT);
	const uint32_t normalOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
	const uint32_t tangentOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL);

	auto encodeSnorm8 = [](float v) -> int8_t {
		return static_cast<int8_t>(std::clamp(v * 127.0f, -127.0f, 127.0f));
	};
	auto normalize = [](RE::NiPoint3 v) -> RE::NiPoint3 {
		float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		return len > 1e-6f ? RE::NiPoint3{ v.x / len, v.y / len, v.z / len } : RE::NiPoint3{ 0, 0, 1 };
	};

	// First pass: compute totals and find bounding box
	uint32_t totalVerts = 0;
	uint32_t totalIndices = 0;
	float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
	float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;

	uint32_t sourcesMerged = 0;
	for (uint32_t i = startIndex; i < sources.size(); i++) {
		auto& src = sources[i];
		if (totalVerts + src.vertexCount > 65535)
			break;
		totalVerts += src.vertexCount;
		totalIndices += src.triangleCount * 3;
		sourcesMerged++;
	}

	if (totalVerts < 3 || totalIndices < 3 || sourcesMerged == 0)
		return nullptr;

	// Allocate combined buffers
	eastl::vector<uint8_t> combinedVB(totalVerts * vertexStride);
	eastl::vector<uint16_t> combinedIB(totalIndices);

	// Second pass: merge vertices and indices
	uint32_t vertOffset = 0;
	uint32_t idxOffset = 0;

	for (uint32_t srcIdx = startIndex; srcIdx < startIndex + sourcesMerged; srcIdx++) {
		auto& src = sources[srcIdx];
		if (vertOffset + src.vertexCount > totalVerts)
			break;

		const uint8_t* srcVerts = src.rendererData->rawVertexData;
		const uint16_t* srcIndices = src.rendererData->rawIndexData;
		const auto& world = src.geometry->world;
		uint32_t srcIndexCount = src.triangleCount * 3;

		for (uint32_t v = 0; v < src.vertexCount; v++) {
			uint8_t* dst = combinedVB.data() + (vertOffset + v) * vertexStride;
			const uint8_t* srcV = srcVerts + v * vertexStride;
			std::memcpy(dst, srcV, vertexStride);

			float* pos = reinterpret_cast<float*>(dst);

			// Read all model-space values from the freshly-copied vertex BEFORE writing
			// anything back. The bitangent is split across pos.w / normal.w / tangent.w.
			RE::NiPoint3 posLocal{ pos[0], pos[1], pos[2] };
			float bitX_local = pos[3];

			int8_t* nBytes = nullptr;
			int8_t* tBytes = nullptr;
			RE::NiPoint3 normalLocal{};
			RE::NiPoint3 tangentLocal{};
			float bitY_local = 0.0f;
			float bitZ_local = 0.0f;

			if (hasNormal) {
				nBytes = reinterpret_cast<int8_t*>(dst + normalOffset);
				normalLocal = { nBytes[0] / 127.0f, nBytes[1] / 127.0f, nBytes[2] / 127.0f };
				bitY_local = nBytes[3] / 127.0f;
			}
			if (hasTangent) {
				tBytes = reinterpret_cast<int8_t*>(dst + tangentOffset);
				tangentLocal = { tBytes[0] / 127.0f, tBytes[1] / 127.0f, tBytes[2] / 127.0f };
				bitZ_local = tBytes[3] / 127.0f;
			}

			// Transform position (point: rotate + scale + translate)
			RE::NiPoint3 worldPos = world.rotate * (posLocal * world.scale) + world.translate;
			pos[0] = worldPos.x;
			pos[1] = worldPos.y;
			pos[2] = worldPos.z;

			minX = std::min(minX, worldPos.x);
			minY = std::min(minY, worldPos.y);
			minZ = std::min(minZ, worldPos.z);
			maxX = std::max(maxX, worldPos.x);
			maxY = std::max(maxY, worldPos.y);
			maxZ = std::max(maxZ, worldPos.z);

			// Transform tangent frame (direction vectors: rotate only, then renormalize).
			// Uniform scale doesn't change directions; non-uniform statics are rare and the
			// renormalize keeps non-uniform cases approximately correct without an inverse-transpose.
			if (hasNormal) {
				auto N = normalize(world.rotate * normalLocal);
				nBytes[0] = encodeSnorm8(N.x);
				nBytes[1] = encodeSnorm8(N.y);
				nBytes[2] = encodeSnorm8(N.z);

				if (hasTangent) {
					auto T = normalize(world.rotate * tangentLocal);
					auto B = normalize(world.rotate * RE::NiPoint3{ bitX_local, bitY_local, bitZ_local });
					tBytes[0] = encodeSnorm8(T.x);
					tBytes[1] = encodeSnorm8(T.y);
					tBytes[2] = encodeSnorm8(T.z);
					pos[3] = B.x;
					nBytes[3] = encodeSnorm8(B.y);
					tBytes[3] = encodeSnorm8(B.z);
				}
			}
		}

		for (uint32_t i = 0; i < srcIndexCount; i++)
			combinedIB[idxOffset + i] = static_cast<uint16_t>(srcIndices[i] + vertOffset);

		vertOffset += src.vertexCount;
		idxOffset += srcIndexCount;
	}

	// Compute combine origin (center of AABB) and shift vertices to local space
	RE::NiPoint3 combineOrigin{
		(minX + maxX) * 0.5f,
		(minY + maxY) * 0.5f,
		(minZ + maxZ) * 0.5f
	};

	for (uint32_t v = 0; v < totalVerts; v++) {
		float* pos = reinterpret_cast<float*>(combinedVB.data() + v * vertexStride);
		pos[0] -= combineOrigin.x;
		pos[1] -= combineOrigin.y;
		pos[2] -= combineOrigin.z;
	}

	float boundRadius = std::sqrt(
		(maxX - minX) * (maxX - minX) +
		(maxY - minY) * (maxY - minY) +
		(maxZ - minZ) * (maxZ - minZ)) * 0.5f;

	// ---- Engine factory: Renderer::CreateTriShape ----
	using Renderer_CreateTriShape_t = RE::BSGraphics::TriShape* (*)(
		RE::BSGraphics::Renderer*,
		void*, uint32_t, uint64_t, void*, uint32_t);
	static const REL::Relocation<Renderer_CreateTriShape_t>
		Renderer_CreateTriShape{ REL::Offset(0xE45F70) };

	auto* rd = Renderer_CreateTriShape(
		renderer,
		combinedVB.data(),
		static_cast<uint32_t>(totalVerts * vertexStride),
		key.vertexDesc,
		combinedIB.data(),
		totalIndices);
	if (!rd)
		return nullptr;

	// ---- Engine factory: BSTriShape::Create ----
	using BSTriShape_Create_t = RE::BSTriShape* (*)();
	static const REL::Relocation<BSTriShape_Create_t>
		BSTriShape_Create{ REL::Offset(0xD2D730) };

	auto* tri = BSTriShape_Create();
	if (!tri)
		return nullptr;

	// Wire BSGraphics::TriShape onto BSGeometry
	auto& geomData = tri->GetGeometryRuntimeData();
	geomData.rendererData = rd;
	std::memcpy(&geomData.vertexDesc, &key.vertexDesc, sizeof(uint64_t));

	auto& triData = tri->GetTrishapeRuntimeData();
	triData.vertexCount = static_cast<uint16_t>(totalVerts);
	triData.triangleCount = static_cast<uint16_t>(totalIndices / 3);

	// Model bound for frustum culling
	auto& modelData = tri->GetModelData();
	modelData.modelBound.center = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
	modelData.modelBound.radius = boundRadius;

	// ---- BSLightingShaderProperty: clone from first merged source ----
	auto* srcLighting = netimmerse_cast<RE::BSLightingShaderProperty*>(
		sources[startIndex].geometry->GetGeometryRuntimeData().shaderProperty.get());
	if (srcLighting && srcLighting->material) {
		using BSLightingShaderProperty_Ctor_t =
			RE::BSShaderProperty* (*)(RE::BSLightingShaderProperty*);
		static const REL::Relocation<BSLightingShaderProperty_Ctor_t>
			BSLightingShaderProperty_Ctor{ REL::Offset(0x14ACC20) };

		auto* prop = static_cast<RE::BSLightingShaderProperty*>(
			memmgr->Allocate(sizeof(RE::BSLightingShaderProperty), 0, false));
		if (prop) {
			BSLightingShaderProperty_Ctor(prop);
			prop->SetMaterial(srcLighting->material, false);
			prop->flags = srcLighting->flags;
			prop->alpha = srcLighting->alpha;
			prop->emissiveMult = srcLighting->emissiveMult;
			prop->specularLODFade = srcLighting->specularLODFade;
			prop->envmapLODFade = srcLighting->envmapLODFade;

			geomData.shaderProperty = RE::NiPointer<RE::BSShaderProperty>(prop);
		}
	}

	// Cell-level parent has identity rotation and unit scale.
	// Vertices are world-space (camera-relative) minus combineOrigin,
	// so local.translate positions the mesh at combineOrigin in parent's space.
	tri->local.translate = combineOrigin - parent->world.translate;

	tri->name = "CS_MeshCombine";

	// Attach to scene graph
	parent->AttachChild(tri, true);
	RE::NiUpdateData updateData;
	parent->UpdateUpwardPass(updateData);
	parent->UpdateDownwardPass(updateData, 0);

	stat_totalVerticesCombined += totalVerts;
	outSourcesMerged = sourcesMerged;

	return RE::NiPointer<RE::BSTriShape>(tri);
}

void MeshCombiner::DestroyCombines()
{
	SetOriginalVisibility(true);

	for (auto& ac : activeCombines) {
		if (ac.triShape && ac.parent) {
			ac.parent->DetachChild(ac.triShape.get());
		}
	}
	activeCombines.clear();

	stat_totalCombines = 0;
	stat_totalSourceMeshes = 0;
	stat_totalVerticesCombined = 0;
	stat_groupsFound = 0;
}

void MeshCombiner::PruneDetachedCombines()
{
	activeCombines.erase(
		eastl::remove_if(activeCombines.begin(), activeCombines.end(),
			[](const ActiveCombine& ac) {
				if (!ac.triShape || !ac.parent)
					return true;
				for (auto& childPtr : ac.parent->GetChildren()) {
					if (childPtr.get() == ac.triShape.get())
						return false;
				}
				return true;
			}),
		activeCombines.end());
}

void MeshCombiner::SetOriginalVisibility(bool visible)
{
	for (auto& ac : activeCombines) {
		for (auto* geom : ac.sourceGeometries) {
			if (geom)
				geom->SetAppCulled(!visible);
		}
	}
}

// ---- Settings UI ----

void MeshCombiner::DrawSettings()
{
	bool changed = false;
	changed |= ImGui::Checkbox("Enable", &settings.Enable);

	if (!settings.Enable) {
		if (changed && !activeCombines.empty())
			DestroyCombines();
		return;
	}

	if (ImGui::Checkbox("Hide Originals", &settings.HideOriginals)) {
		if (!activeCombines.empty())
			SetOriginalVisibility(!settings.HideOriginals);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("AppCull source meshes so only combined draws render");

	uint32_t minVal = 2, maxVal = 32;
	if (ImGui::SliderScalar("Min Group Size", ImGuiDataType_U32, &settings.MinGroupSize, &minVal, &maxVal,
			"%u", ImGuiSliderFlags_AlwaysClamp))
		combinesDirty = true;

	ImGui::Separator();
	ImGui::Text("Statistics:");
	ImGui::Text("  Material groups found: %u", stat_groupsFound);
	ImGui::Text("  Combined meshes: %u", stat_totalCombines);
	ImGui::Text("  Source meshes merged: %u", stat_totalSourceMeshes);
	ImGui::Text("  Total vertices: %u", stat_totalVerticesCombined);

	if (ImGui::Button("Force Rebuild"))
		combinesDirty = true;
}

void MeshCombiner::LoadSettings(json& o_json)
{
	settings.Enable = o_json.value("Enable", settings.Enable);
	settings.HideOriginals = o_json.value("HideOriginals", settings.HideOriginals);
	settings.MinGroupSize = o_json.value("MinGroupSize", settings.MinGroupSize);
}

void MeshCombiner::SaveSettings(json& o_json)
{
	o_json["Enable"] = settings.Enable;
	o_json["HideOriginals"] = settings.HideOriginals;
	o_json["MinGroupSize"] = settings.MinGroupSize;
}

void MeshCombiner::RestoreDefaultSettings()
{
	settings = {};
}
