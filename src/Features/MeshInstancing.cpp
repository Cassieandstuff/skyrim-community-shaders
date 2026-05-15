#include "MeshInstancing.h"

#include "Globals.h"
#include "Hooks.h"
#include "State.h"

bool MeshInstancing::HasShaderDefine(RE::BSShader::Type shaderType)
{
	return shaderType == RE::BSShader::Type::Lighting;
}

// ---- Feature lifecycle ----

void MeshInstancing::SetupResources()
{
	auto device = globals::d3d::device;
	if (!device)
		return;

	auto desc = StructuredBufferDesc<InstanceTransform>(settings.MaxTotalInstances);
	instanceBuffer = eastl::make_unique<StructuredBuffer>(desc, settings.MaxTotalInstances, "MeshInstancing::InstanceBuffer");
	instanceBuffer->CreateSRV();

	logger::info("[MESH INSTANCING] Created instance buffer for {} max instances ({} bytes each)",
		settings.MaxTotalInstances, sizeof(InstanceTransform));
}

void MeshInstancing::Reset()
{
	currentLightingGeometry = nullptr;
	inLightingShader = false;
}

void MeshInstancing::Prepass()
{
	if (!settings.Enabled)
		return;

	// Reset per-frame stats and flags here (after UI has read them, before new draws)
	for (auto& [key, group] : instanceGroups)
		group.drawnThisFrame = false;
	stat_instancedDrawCalls = 0;
	stat_suppressedDrawCalls = 0;
	stat_totalInstancesRendered = 0;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (player) {
		auto* cell = player->GetParentCell();
		auto cellID = cell ? cell->GetFormID() : 0;
		if (cellID != lastPlayerCellID) {
			logger::info("[MESH INSTANCING] Cell change detected: {:08X} -> {:08X}", lastPlayerCellID, cellID);
			lastPlayerCellID = cellID;
			groupsDirty = true;
		}
	}

	if (groupsDirty) {
		if (RebuildInstanceGroups())
			groupsDirty = false;
	}

	UploadInstanceTransforms();
}

void MeshInstancing::DataLoaded()
{
	Hooks::Install();
	groupsDirty = true;
}

void MeshInstancing::ClearShaderCache()
{
}

// ---- Instance group management ----

bool MeshInstancing::RebuildInstanceGroups()
{
	ZoneScoped;

	instanceGroups.clear();
	geometryToGroup.clear();

	auto* tes = RE::TES::GetSingleton();
	if (!tes) {
		logger::info("[MESH INSTANCING] RebuildInstanceGroups: tes is null");
		return false;
	}

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player) {
		logger::info("[MESH INSTANCING] RebuildInstanceGroups: player is null");
		return false;
	}

	auto* currentCell = player->GetParentCell();
	if (!currentCell) {
		logger::info("[MESH INSTANCING] RebuildInstanceGroups: currentCell is null");
		return false;
	}

	if (currentCell->IsInteriorCell()) {
		ScanCell(currentCell);
	} else {
		if (auto* gridCells = tes->gridCells) {
			auto gridLength = gridCells->length;
			logger::info("[MESH INSTANCING] Exterior scan: gridLength={}", gridLength);
			if (gridLength > 0) {
				for (uint32_t x = 0; x < gridLength; x++) {
					for (uint32_t y = 0; y < gridLength; y++) {
						if (auto* cell = gridCells->GetCell(x, y))
							ScanCell(cell);
					}
				}
			}
		} else {
			logger::info("[MESH INSTANCING] RebuildInstanceGroups: gridCells is null");
		}
	}

	// Prune groups below the minimum instance threshold
	eastl::vector<MeshKey> toRemove;
	for (auto& [key, group] : instanceGroups) {
		if (group.geometries.size() < settings.MinInstanceCount)
			toRemove.push_back(key);
	}
	for (auto& key : toRemove) {
		auto it = instanceGroups.find(key);
		if (it != instanceGroups.end()) {
			for (auto* geom : it->second.geometries)
				geometryToGroup.erase(geom);
			instanceGroups.erase(it);
		}
	}

	// Assign buffer offsets and enforce per-group and total caps
	uint32_t currentOffset = 0;
	eastl::vector<MeshKey> overCapacity;
	for (auto& [key, group] : instanceGroups) {
		auto count = static_cast<uint32_t>(group.geometries.size());
		if (count > settings.MaxInstancesPerGroup)
			count = settings.MaxInstancesPerGroup;
		if (currentOffset + count > settings.MaxTotalInstances) {
			overCapacity.push_back(key);
			continue;
		}
		group.bufferOffset = currentOffset;
		currentOffset += count;
	}
	for (auto& key : overCapacity) {
		auto it = instanceGroups.find(key);
		if (it != instanceGroups.end()) {
			for (auto* geom : it->second.geometries)
				geometryToGroup.erase(geom);
			instanceGroups.erase(it);
		}
	}

	stat_totalGroups = static_cast<uint32_t>(instanceGroups.size());

	uint32_t totalInstances = 0;
	for (auto& [key, group] : instanceGroups)
		totalInstances += static_cast<uint32_t>(group.geometries.size());

	logger::info("[MESH INSTANCING] Rebuilt: {} groups, {} total instances (cell: {:08X}, interior: {})",
		stat_totalGroups, totalInstances,
		currentCell->GetFormID(),
		currentCell->IsInteriorCell());

	return true;
}

void MeshInstancing::ScanCell(RE::TESObjectCELL* cell)
{
	if (!cell)
		return;

	uint32_t refsScanned = 0;
	uint32_t geomsFound = 0;

	cell->ForEachReference([&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
		if (!ref || ref->IsDisabled() || ref->IsDeleted())
			return RE::BSContainer::ForEachResult::kContinue;

		auto* baseObj = ref->GetBaseObject();
		if (!baseObj || baseObj->GetFormType() != RE::FormType::Static)
			return RE::BSContainer::ForEachResult::kContinue;

		auto* node3D = ref->Get3D();
		if (!node3D)
			return RE::BSContainer::ForEachResult::kContinue;

		refsScanned++;

		RE::BSVisit::TraverseScenegraphGeometries(node3D, [&](RE::BSGeometry* geometry) -> RE::BSVisit::BSVisitControl {
			if (!geometry)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto& runtimeData = geometry->GetGeometryRuntimeData();

			// Skip skinned meshes
			if (runtimeData.skinInstance)
				return RE::BSVisit::BSVisitControl::kContinue;

			auto* triShape = runtimeData.rendererData;
			if (!triShape)
				return RE::BSVisit::BSVisitControl::kContinue;

			MeshKey key{ triShape };
			auto& group = instanceGroups[key];
			group.geometries.push_back(geometry);
			geometryToGroup[geometry] = key;
			geomsFound++;

			return RE::BSVisit::BSVisitControl::kContinue;
		});

		return RE::BSContainer::ForEachResult::kContinue;
	});

	logger::info("[MESH INSTANCING] ScanCell '{}': {} static refs, {} geometries",
		cell->GetFormEditorID(), refsScanned, geomsFound);
}

void MeshInstancing::UploadInstanceTransforms()
{
	if (instanceGroups.empty() || !instanceBuffer)
		return;

	// Build CPU-side staging data, then upload in one shot
	eastl::vector<InstanceTransform> staging;
	staging.resize(settings.MaxTotalInstances);

	for (auto& [key, group] : instanceGroups) {
		auto count = std::min(static_cast<uint32_t>(group.geometries.size()), settings.MaxInstancesPerGroup);
		for (uint32_t i = 0; i < count; i++) {
			auto* geom = group.geometries[i];
			if (!geom)
				continue;

			auto& world = geom->world;
			auto& transform = staging[group.bufferOffset + i];

			// NiTransform → 3 x float4 rows matching HLSL row_major float3x4 (scale baked into rotation)
			float s = world.scale;
			auto& r = world.rotate.entry;
			auto& t = world.translate;
			transform.World[0] = float4(r[0][0] * s, r[0][1] * s, r[0][2] * s, t.x);
			transform.World[1] = float4(r[1][0] * s, r[1][1] * s, r[1][2] * s, t.y);
			transform.World[2] = float4(r[2][0] * s, r[2][1] * s, r[2][2] * s, t.z);

			// Statics don't move — previous == current (zero motion vectors)
			transform.PreviousWorld[0] = transform.World[0];
			transform.PreviousWorld[1] = transform.World[1];
			transform.PreviousWorld[2] = transform.World[2];
		}
	}

	instanceBuffer->Update(staging.data(), staging.size() * sizeof(InstanceTransform));
}

// ---- Render-time ----

bool MeshInstancing::HandleDraw(RE::BSGeometry* geometry, uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
{
	if (!settings.Enabled || !geometry)
		return false;

	auto it = geometryToGroup.find(geometry);
	if (it == geometryToGroup.end())
		return false;

	auto groupIt = instanceGroups.find(it->second);
	if (groupIt == instanceGroups.end())
		return false;

	auto& group = groupIt->second;

	if (group.drawnThisFrame) {
		stat_suppressedDrawCalls++;
		return true;
	}

	group.drawnThisFrame = true;

	auto context = globals::d3d::context;
	auto instanceCount = std::min(static_cast<uint32_t>(group.geometries.size()), settings.MaxInstancesPerGroup);
	if (settings.DebugSingleInstance)
		instanceCount = 1;

	auto* srv = instanceBuffer->SRV();
	context->VSSetShaderResources(InstanceBufferSRVSlot, 1, &srv);

	auto* state = globals::state;
	auto savedExtra = state->permutationData.ExtraShaderDescriptor;
	state->permutationData.ExtraShaderDescriptor =
		(savedExtra & 0x7F) |
		static_cast<uint32_t>(State::ExtraShaderDescriptors::IsInstanced) |
		(group.bufferOffset << 7);
	state->permutationCB->Update(state->permutationData);

	context->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, 0);

	state->permutationData.ExtraShaderDescriptor = savedExtra;
	state->permutationCB->Update(state->permutationData);

	stat_instancedDrawCalls++;
	stat_totalInstancesRendered += instanceCount;

	return true;
}

// ---- Hooks ----

void MeshInstancing::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);

	auto& instancing = globals::features::meshInstancing;
	if (!instancing.loaded || !instancing.settings.Enabled)
		return;

	instancing.inLightingShader = true;
	instancing.currentLightingGeometry = Pass ? Pass->geometry : nullptr;
}

void STDMETHODCALLTYPE MeshInstancing::Hooks::ID3D11DeviceContext_DrawIndexed::thunk(
	ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	auto& instancing = globals::features::meshInstancing;

	if (instancing.loaded && instancing.inLightingShader && instancing.currentLightingGeometry) {
		if (instancing.HandleDraw(instancing.currentLightingGeometry, IndexCount, StartIndexLocation, BaseVertexLocation)) {
			instancing.inLightingShader = false;
			instancing.currentLightingGeometry = nullptr;
			return;
		}
	}

	instancing.inLightingShader = false;
	instancing.currentLightingGeometry = nullptr;

	func(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void MeshInstancing::Hooks::Install()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[MESH INSTANCING] Installed BSLightingShader::SetupGeometry hook");
}

// ---- Settings UI ----

void MeshInstancing::DrawSettings()
{
	ImGui::Checkbox("Enable Mesh Instancing", &settings.Enabled);
	ImGui::Checkbox("Debug: Single Instance", &settings.DebugSingleInstance);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Draw with instanceCount=1 to isolate multi-instance issues");

	uint32_t minVal = 2, maxVal = 16;
	ImGui::SliderScalar("Min Instance Count", ImGuiDataType_U32, &settings.MinInstanceCount, &minVal, &maxVal,
		"%u", ImGuiSliderFlags_AlwaysClamp);

	ImGui::Separator();
	ImGui::Text("Statistics (last frame):");
	ImGui::Text("  Instance groups: %u", stat_totalGroups);
	ImGui::Text("  Instanced draws: %u (%u total instances)", stat_instancedDrawCalls, stat_totalInstancesRendered);
	ImGui::Text("  Suppressed draws: %u", stat_suppressedDrawCalls);
}

void MeshInstancing::LoadSettings(json& o_json)
{
	settings.Enabled = o_json.value("Enabled", settings.Enabled);
	settings.MinInstanceCount = o_json.value("MinInstanceCount", settings.MinInstanceCount);
	settings.MaxInstancesPerGroup = o_json.value("MaxInstancesPerGroup", settings.MaxInstancesPerGroup);
	settings.MaxTotalInstances = o_json.value("MaxTotalInstances", settings.MaxTotalInstances);
}

void MeshInstancing::SaveSettings(json& o_json)
{
	o_json["Enabled"] = settings.Enabled;
	o_json["MinInstanceCount"] = settings.MinInstanceCount;
	o_json["MaxInstancesPerGroup"] = settings.MaxInstancesPerGroup;
	o_json["MaxTotalInstances"] = settings.MaxTotalInstances;
}

void MeshInstancing::RestoreDefaultSettings()
{
	settings = {};
}
