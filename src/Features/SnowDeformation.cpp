#include "Globals.h"
#include "SceneHeight.h"
#include "SnowDeformation.h"

#include "Deferred.h"
#include "State.h"
#include "Features/GrassCollision.h"
#include "RE/B/BSLightingShaderMaterialBase.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "RE/N/NiNode.h"
#include "RE/T/TESObjectLAND.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowDeformation::Settings,
	Enable,
	SnowLayerDepth,
	SnowContactDepth,
	SettlingRate,
	RidgeStrength,
	SnowAltitudeMin,
	TerrainSurfaceEpsilon,
	DebugForceDeform,
	BuildAndAttachMesh)

bool SnowDeformation::HasShaderDefine(RE::BSShader::Type shaderType)
{
	(void)shaderType;
	return false;
}

// =============================================================================
// GetCommonBufferData
//   Called by the State system each frame to populate the FeatureData cbuffer.
//   Pure read — all grid-state mutations happen in Prepass() so this function
//   is safe to call any number of times per frame without side effects.
// =============================================================================
SnowDeformation::CommonBufferData SnowDeformation::GetCommonBufferData()
{
	CommonBufferData data{};
	data.Enabled = (settings.Enable && loaded) ? 1u : 0u;

	if (!data.Enabled)
		return data;

	// Read the grid state computed by Prepass() this frame.
	data.GridWorldSize    = GRID_WORLD_SIZE;
	data.GridCellSize     = GRID_CELL_SIZE;
	data.RcpGridCellSize  = 1.0f / GRID_CELL_SIZE;
	data.GridWorldOriginX = currentGridWorldOriginX;
	data.GridWorldOriginY = currentGridWorldOriginY;
	data.ArrayOriginX     = currentArrayOriginX;
	data.ArrayOriginY     = currentArrayOriginY;

	data.SnowLayerDepth        = settings.SnowLayerDepth;
	data.SnowContactDepth      = settings.SnowContactDepth;
	data.SettlingRate          = settings.SettlingRate;
	data.RidgeStrength         = settings.RidgeStrength;
	data.SnowAltitudeMin       = settings.SnowAltitudeMin;
	data.TerrainSurfaceEpsilon = settings.TerrainSurfaceEpsilon;
	data.DebugForceDeform      = settings.DebugForceDeform ? 1u : 0u;
	data.Reserved              = 1;

	float dt = *globals::game::deltaTime * !globals::game::ui->GameIsPaused();
	data.DeltaTime = dt;

	return data;
}

// =============================================================================
// UpdateTerrainHeight
//   Progressive fill: TERRAIN_FILL_ROWS_PER_FRAME rows on first-fill pass, then
//   only newly-exposed rows/columns when the camera moves.
// =============================================================================
void SnowDeformation::UpdateTerrainHeight()
{
	if (!texTerrainHeight)
		return;

	auto tes     = globals::game::tes;
	auto context = globals::d3d::context;

	if (!tes)
		return;

	bool updated = false;

	// ---- Progressive initial fill ----
	if (terrainFillRow < GRID_DIM) {
		for (uint32_t r = 0; r < TERRAIN_FILL_ROWS_PER_FRAME && terrainFillRow < GRID_DIM; r++, terrainFillRow++) {
			for (uint32_t c = 0; c < GRID_DIM; c++) {
				int localX = ((int)c          - currentArrayOriginX % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
				int localY = ((int)terrainFillRow - currentArrayOriginY % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

				float worldX = currentGridWorldOriginX + (localX + 0.5f) * GRID_CELL_SIZE;
				float worldY = currentGridWorldOriginY + (localY + 0.5f) * GRID_CELL_SIZE;

				float      height = 0.0f;
				RE::NiPoint3 pos(worldX, worldY, 0.0f);
				tes->GetLandHeight(pos, height);

				terrainHeightCPU[terrainFillRow * GRID_DIM + c] = height;
			}
		}
		updated = true;
		terrainInitialized = terrainFillRow >= GRID_DIM;

	} else {
		// ---- Incremental update for newly-exposed cells after camera scroll ----
		int absMarginX = std::abs(currentValidMarginX);
		int absMarginY = std::abs(currentValidMarginY);

		// Large teleport: restart full fill
		if (absMarginX > (int)GRID_DIM / 2 || absMarginY > (int)GRID_DIM / 2) {
			terrainFillRow = 0;
			terrainInitialized = false;
			return;
		}

		for (uint32_t row = 0; row < GRID_DIM; row++) {
			for (uint32_t col = 0; col < GRID_DIM; col++) {
				int localX = ((int)col - currentArrayOriginX % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
				int localY = ((int)row - currentArrayOriginY % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

				bool isNew = false;
				// currentValidMarginX = prevCellIDX - cellIDX
				//   < 0  →  camera moved in +X  →  new cells appear at the RIGHT edge (localX near GRID_DIM-1)
				//   > 0  →  camera moved in -X  →  new cells appear at the LEFT  edge (localX near 0)
				if (currentValidMarginX < 0 && localX >= (int)GRID_DIM + currentValidMarginX) isNew = true;
				if (currentValidMarginX > 0 && localX <  currentValidMarginX)                 isNew = true;
				if (currentValidMarginY < 0 && localY >= (int)GRID_DIM + currentValidMarginY) isNew = true;
				if (currentValidMarginY > 0 && localY <  currentValidMarginY)                 isNew = true;

				if (isNew) {
					float worldX = currentGridWorldOriginX + (localX + 0.5f) * GRID_CELL_SIZE;
					float worldY = currentGridWorldOriginY + (localY + 0.5f) * GRID_CELL_SIZE;

					float      height = 0.0f;
					RE::NiPoint3 pos(worldX, worldY, 0.0f);
					tes->GetLandHeight(pos, height);

					terrainHeightCPU[row * GRID_DIM + col] = height;
					updated = true;
				}
			}
		}
	}

	if (updated) {
		context->UpdateSubresource(
			texTerrainHeight->resource.get(), 0, nullptr,
			terrainHeightCPU.data(),
			GRID_DIM * sizeof(float), 0);
	}
}

// =============================================================================
// DeformationPass
//   Called from Deferred::DeferredPasses() after the geometry pass.
//   Dispatches the SnowDeformCS which reads the GrassCollision actor-contact
//   heightfield (t5) and the terrain height field (t2) to update the
//   deformation field, then increments simFrameIndex.
// =============================================================================
void SnowDeformation::DeformationPass()
{
	if (!loaded || !settings.Enable)
		return;
	if (!snowDeformCS || !texSnowDeform[0] || !texSnowDeform[1] || !texSnowRidge || !texTerrainHeight)
		return;
	if (!terrainInitialized)
		return;

	auto context  = globals::d3d::context;

	uint32_t readIdx  = simFrameIndex % 2;
	uint32_t writeIdx = (simFrameIndex + 1) % 2;

	// ---- Bind cbuffers ----
	ID3D11Buffer* sharedBufs[2] = {
		globals::state->sharedDataCB->CB(),
		globals::state->featureDataCB->CB()
	};
	context->CSSetConstantBuffers(5, 2, sharedBufs);

	ID3D11Buffer* perFrameBuf = *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrameBuf);

	// ---- Bind input SRVs ----
	auto& grassCollision = globals::features::grassCollision;
	ID3D11ShaderResourceView* grassSRV = (grassCollision.loaded && grassCollision.collisionTexture)
		? grassCollision.collisionTexture->srv.get() : nullptr;

	ID3D11ShaderResourceView* srvs[4] = {
		texTerrainHeight->srv.get(),         // t2 — terrain absolute Z
		texSnowDeform[readIdx]->srv.get(),   // t3 — previous frame deformation
		nullptr,                             // t4 — unused
		grassSRV                             // t5 — GrassCollision actor-contact heightfield
	};
	context->CSSetShaderResources(2, 4, srvs);  // slots t2, t3, t4, t5

	// ---- Bind output UAVs ----
	ID3D11UnorderedAccessView* uavs[2] = {
		texSnowDeform[writeIdx]->uav.get(),  // u0 — current deformation
		texSnowRidge->uav.get()              // u1 — ridge magnitude
	};
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

	// ---- Dispatch ----
	context->CSSetShader(snowDeformCS.get(), nullptr, 0);
	context->Dispatch(GRID_DIM / 8, GRID_DIM / 8, 1);

	// ---- Unbind ----
	ID3D11ShaderResourceView*  nullSRVs[4]  = {};
	ID3D11UnorderedAccessView* nullUAVs[2]  = {};
	context->CSSetShaderResources(2, 4, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	simFrameIndex++;
}

// =============================================================================
// OnLandSetupMaterial
//   Detoured TESObjectLAND::SetupMaterial fires here, once per cell-quad,
//   after the engine has filled LoadedLandData (heights, normals, colors,
//   per-quad shaderProperty).  For each of the 4 quads we synthesise a snow
//   heightmap (= terrain heights + SnowLayerDepth) and call BuildSnowQuadMesh
//   to produce a fresh, independent BSTriShape patterned after the engine's
//   global terrain mesh.  No cloning, no terrain identity, no shared GPU
//   resources with terrain.
//
//   Once BuildSnowQuadMesh returns a non-null mesh (it's a stub right now),
//   subsequent work is: parent it under our own NiNode at the cell's quad
//   position, register for cleanup on cell unload, and (M2) hand its VB to
//   the deformation CS for per-vertex Z updates.
// =============================================================================
void SnowDeformation::OnLandSetupMaterial(RE::TESObjectLAND* land)
{
	if (!land || !land->loadedData)
		return;
	if (!loaded || !settings.Enable)
		return;

	auto* data = land->loadedData;
	// `settings.SnowLayerDepth` is consumed downstream by UpdateSlabVerticesIfNeeded
	// when it computes per-vertex Z from the SceneHeight CPU mirror.  No
	// longer needed at slab-construction time since initial Z is just a
	// placeholder zero (slab stays triangleCount=0 until that update lands).

	std::lock_guard<std::mutex> lk(snowMeshMutex);

	// Throttle per-call logging — cell load is bursty.
	static int s_logCount = 0;
	auto       shouldDiagLog = [&]() {
		if (s_logCount >= 32) {
			return false;
		}

		++s_logCount;
		return true;
	};

	for (int q = 0; q < 4; ++q) {
		// Per-vertex Z for the slab is no longer computed here.  Slab vertices
		// are seeded with placeholder Z=0 by BuildSnowQuadMesh; the real
		// heightmap-following values are written by
		// SnowDeformation::UpdateSlabVerticesIfNeeded once SceneHeight's CPU
		// mirrors finish their readback.  Until then the slab stays
		// triangleCount=0 (invisible), so the placeholder never renders.

		// Master gate.  When off, hook + heightmap math run but nothing
		// touches the engine — useful for isolating regressions.
		if (!settings.BuildAndAttachMesh) {
			if (shouldDiagLog()) {
				logger::info("[SnowDeformation] quad {}: BuildAndAttachMesh=false — dry-run only", q);
			}
			continue;
		}

		RE::NiNode* parent = data->mesh[q];
		if (!parent) {
			if (shouldDiagLog()) {
				logger::info("[SnowDeformation] quad {}: no parent NiNode", q);
			}
			continue;
		}

		// DIAGNOSTIC: capture pre-attach state per quad to understand the
		// every-other-quad gap.  Theories under test:
		//   1. data->mesh[0..3] are aliased to the same NiNode (dedup-by-pointer)
		//   2. Only data->geom[3] has a non-identity local; others are at origin
		//   3. data->mesh[q] world transform varies per quad in unexpected ways
		// All quads + all geoms logged unconditionally for first cell load,
		// then throttled by shouldDiagLog().
		if (shouldDiagLog()) {
			auto* geomQ = data->geom[q].get();
			logger::info(
				"[SnowDeformation] DIAG quad {} (cell-pre-attach):\n"
				"    mesh[{}]    addr={}     world.translate=({:.1f}, {:.1f}, {:.1f})\n"
				"    geom[{}]    addr={}     local.translate=({:.1f}, {:.1f}, {:.1f})",
				q,
				q, static_cast<void*>(parent),
				parent->world.translate.x, parent->world.translate.y, parent->world.translate.z,
				q, static_cast<void*>(geomQ),
				geomQ ? geomQ->local.translate.x : 0.0f,
				geomQ ? geomQ->local.translate.y : 0.0f,
				geomQ ? geomQ->local.translate.z : 0.0f);
		}

		// Dedup: per-quad name so each quad gets its own slab even when
		// data->mesh[q] is a shared NiNode across multiple quads (engine
		// consolidates terrain quads in some cells — also seen with
		// data->geom[0..2] having null shaderProperty while geom[3] has it).
		// Using a single "CS_SnowSlab" name caused only the first quad to
		// attach per shared parent, leaving the other 3 quads as gaps.
		static constexpr const char* kQuadNames[4] = {
			"CS_SnowSlab_0",
			"CS_SnowSlab_1",
			"CS_SnowSlab_2",
			"CS_SnowSlab_3",
		};
		const RE::BSFixedString kSnowName{ kQuadNames[q] };
		bool                    alreadyAttached = false;
		for (auto& childPtr : parent->GetChildren()) {
			auto* child = childPtr.get();
			if (child && child->name == kSnowName) {
				alreadyAttached = true;
				break;
			}
		}
		if (alreadyAttached)
			continue;

		// Find a valid texture source — only ONE of data->geom[0..3] typically
		// has its shaderProperty populated (empirically always quad 3, but
		// scan defensively).  We use the same source for all 4 of our snow
		// meshes since terrain texture is uniform across the cell.
		RE::BSTriShape* textureSource = nullptr;
		for (int s = 0; s < 4; ++s) {
			if (data->geom[s] && data->geom[s]->GetGeometryRuntimeData().shaderProperty.get()) {
				textureSource = data->geom[s].get();
				break;
			}
		}

		auto mesh = BuildSnowQuadMesh(textureSource);
		if (!mesh) {
			if (shouldDiagLog()) {
				logger::info("[SnowDeformation] quad {}: BuildSnowQuadMesh returned null", q);
			}
			continue;
		}

		mesh->name = kSnowName;

		// Compute per-quad local transform.
		//
		// data->geom[0..3] are usually aliased or have identity local for
		// all but one quad — relying on a per-quad terrain.local copy breaks
		// for 3 of 4 quads (snow stacks at world origin while only the one
		// quad whose geom carries the cell offset lands correctly).
		//
		// Strategy: scan geom[0..3] for ANY quad with a non-identity local
		// (the "anchor"), back out the cell origin using that quad's known
		// in-cell offset, then forward-compute each quad's local from cell
		// origin + standard Skyrim 2x2 quad layout:
		//   q & 1       = x-bit (0 = left, 1 = right)
		//   (q >> 1)& 1 = y-bit (0 = bottom, 1 = top)
		// So quad 0 = (0,0), quad 1 = (2048,0), quad 2 = (0,2048), quad 3 = (2048,2048)
		// within the cell, each 2048×2048 covering one quarter of the 4096×4096 cell.
		{
			// One quad spans 2048 world units (LAND_QUAD_SIZE in the anonymous
			// namespace below — not visible here yet due to declaration order).
			constexpr float kHalfCell = 2048.0f;

			int          anchorIdx   = -1;
			RE::NiPoint3 anchorLocal = {};
			for (int s = 0; s < 4; ++s) {
				auto* g = data->geom[s].get();
				if (!g)
					continue;
				const auto& t = g->local.translate;
				// Heuristic: non-trivial X or Y indicates the engine has placed this quad.
				// Z alone isn't enough — z-offsets are common even for placeholder quads.
				if (std::abs(t.x) > 1.0f || std::abs(t.y) > 1.0f) {
					anchorIdx   = s;
					anchorLocal = t;
					break;
				}
			}

			if (anchorIdx >= 0) {
				const float        anchorOffsetX = static_cast<float>(anchorIdx & 1) * kHalfCell;
				const float        anchorOffsetY = static_cast<float>((anchorIdx >> 1) & 1) * kHalfCell;
				const RE::NiPoint3 cellOrigin{
					anchorLocal.x - anchorOffsetX,
					anchorLocal.y - anchorOffsetY,
					anchorLocal.z
				};

				const float thisQuadOffsetX = static_cast<float>(q & 1) * kHalfCell;
				const float thisQuadOffsetY = static_cast<float>((q >> 1) & 1) * kHalfCell;

				mesh->local.translate = RE::NiPoint3{
					cellOrigin.x + thisQuadOffsetX,
					cellOrigin.y + thisQuadOffsetY,
					cellOrigin.z
				};
			}
			// (If no anchor found, mesh->local stays at identity → world origin,
			// matching the buggy old behaviour.  Would only happen if all 4 geoms
			// are null or identity, which we haven't observed.)
		}

		parent->AttachChild(mesh.get(), true);

		// Register this slab with Layer 3 Stage 1's per-slab update path.
		// `worldOrigin*` = the slab's world XY at its first vertex (after the
		// per-quad anchor-based local placement above).  Captured here so the
		// vertex-update CPU pass can compute each vertex's worldXY without
		// re-deriving from anchor logic.
		{
			RE::NiPoint3 parentWorld = parent->world.translate;
			RegisterSlab(
				mesh.get(),
				parent,
				parentWorld.x + mesh->local.translate.x,
				parentWorld.y + mesh->local.translate.y);
		}

		// Propagate scene-graph state to the freshly-attached mesh.
		// AttachChild alone is just a list operation — it doesn't recompute
		// our worldTransform (= parent.world * mesh.local) or worldBound, nor
		// does it grow the parent's own bound to include us.  Without these
		// updates the engine's per-frame visibility traversal skips the new
		// child entirely.  Mirrors the UnifiedWater pattern (UnifiedWater.cpp:512).
		//
		// Order matters: refresh parent state FIRST via UpdateUpwardPass so its
		// worldTransform reflects whatever ancestor updates have happened since
		// the hook fired; THEN compute our worldTransform from the now-fresh
		// parent state via UpdateDownwardPass.  Without the upward pass, an
		// earlier UpdateWorldData would have computed our world from a stale
		// (still-identity) parent state and we'd render at world origin instead
		// of the cell position.
		{
			RE::NiUpdateData updateData;
			parent->UpdateUpwardPass(updateData);          // refresh parent state from ancestors
			parent->UpdateDownwardPass(updateData, 0);     // propagate parent.world down to us
		}

		if (shouldDiagLog()) {
			logger::info(
				"[SnowDeformation] quad {} POST-ATTACH:\n"
				"    mesh addr={}    vertexCount={}    triCount={}\n"
				"    mesh.local.translate=({:.1f}, {:.1f}, {:.1f})\n"
				"    mesh.world.translate=({:.1f}, {:.1f}, {:.1f})\n"
				"    parent.world.translate=({:.1f}, {:.1f}, {:.1f})",
				q,
				(void*)mesh.get(),
				mesh->GetTrishapeRuntimeData().vertexCount,
				mesh->GetTrishapeRuntimeData().triangleCount,
				mesh->local.translate.x, mesh->local.translate.y, mesh->local.translate.z,
				mesh->world.translate.x, mesh->world.translate.y, mesh->world.translate.z,
				parent->world.translate.x, parent->world.translate.y, parent->world.translate.z);
		}
	}
}

// =============================================================================
// Snow quad mesh build helpers — derived from GlobalLandRenderInit RE +
// VertexDesc::GetSize verification + Ghidra (AE 1170) struct/offset audit.
//
// Our PBR-capable land vertex layout is **32 bytes** packed:
//   - position  @ 0  (16 bytes, float4 — x, y, z, w=1.0)
//   - uv        @ 16 (4 bytes,  int16 × 2, normalized [0, 32767])
//   - normal    @ 20 (4 bytes,  int16 snorm × 2 — xy of unit normal; shader recovers z)
//   - tangent   @ 24 (4 bytes,  int16 snorm × 2 — xy of unit tangent; shader recovers z)
//   - color     @ 28 (4 bytes,  uint8 RGBA)
//
// This matches RE::BSGraphics::VertexDesc::GetSize() exactly for the flag set
// VF_VERTEX | VF_UV | VF_NORMAL | VF_TANGENT | VF_COLORS (0x3B), which is what
// BSGeometry uses to derive vertex stride when binding VBs (CalculateVertexSize
// = (desc<<2)&0x3C → size nibble 8 → 32 bytes).
//
// The 4-byte tangent is what differentiates this from the vanilla 28-byte
// landscape format.  TruePBR routes us through the standard (non-landscape)
// PBR shader path, where `float4 Bitangent: BINORMAL0` (Lighting.hlsl:48) is
// REQUIRED — without it the IA layout has no BINORMAL0 binding, TBN collapses,
// normal-map sampling outputs garbage (the "grey slab" symptom).  For a flat
// horizontal grid the tangent is (1,0,0) at every vertex; trivial to encode.
//
// The engine's terrain mesh layout (single shared mesh at 0x143137288):
//   - 289 vertices arranged 17×17, world-space units 128 apart (1 quad = 2048u)
//   - 1536 uint16 indices forming a triangle list (NOT a strip, despite the
//     name in the engine source) — 16×16 quads × 2 triangles × 3 verts
//   - Per-quad diagonal cut alternates in a checkerboard pattern based on
//     (col + row) % 2
// =============================================================================

namespace
{
	// Slab mesh density.  Each cell-quad covers 2048 world units; slab vertices
	// form an (N+1)×(N+1) grid (N = QUADS_PER_SIDE).  Higher N → finer
	// approximation of the underlying heightmap.
	//
	//   17×17 verts / 16×16 quads → 128 units/vert: coarse, terrain features
	//     under 128 units get averaged to flat planar triangles.
	//   33×33 verts / 32×32 quads → 64 units/vert: 4x more sample points,
	//     captures small rocks and undulations at the heightmap's resolution.
	//   65×65 verts / 64×64 quads → 32 units/vert: matches scene-height
	//     texel spacing (~78 units), diminishing returns past this.
	//
	// 33×33 chosen as the v1 sweet spot: substantial visual improvement vs
	// 17×17, vertex count (1089) fits stack-allocated update buffer easily.
	constexpr int     LAND_VERTS_PER_SIDE = 33;
	constexpr int     LAND_QUADS_PER_SIDE = 32;
	constexpr int     LAND_VERTS          = LAND_VERTS_PER_SIDE * LAND_VERTS_PER_SIDE;        // 1089
	constexpr int     LAND_INDICES        = LAND_QUADS_PER_SIDE * LAND_QUADS_PER_SIDE * 6;    // 6144
	constexpr uint8_t LAND_VERTEX_STRIDE  = 32;
	constexpr float   LAND_QUAD_SIZE      = 2048.0f;
	constexpr float   LAND_VERT_SPACING   = LAND_QUAD_SIZE / LAND_QUADS_PER_SIDE;             // 64 units

	// Texture-tiling factor applied to the material's texCoordScale[0]/[1].  Our
	// vertex UVs span exactly [0, 1] across the 2048-unit quad (the int16-UNORM
	// packing in PackSnowVertex clamps anything else), so without this scale the
	// shader samples a single texture tile stretched across the entire quad —
	// each texel covers ~2 world units, the "warped/smeared" look.
	//
	// The standard lighting VS (Lighting.hlsl:261) does:
	//     uv = input.TexCoord0.xy * TexcoordOffset.zw + TexcoordOffset.xy
	// where TexcoordOffset.zw is fed from material->texCoordScale[0] via the
	// PerMaterial cbuffer.  Empirically with Faultier's PBR snow: 8 → 256 world
	// units per tile sits in the sweet spot.  32 was too busy, 16 still warpy,
	// 8 reads clean at the distances we typically view from.
	constexpr float kSnowUVTileRepeats = 8.0f;

	// VertexDesc bit layout (verified against Ghidra's BSGeometry static
	// helpers + Nukem9's RE + CommonLibSSE-NG VertexDesc.h):
	//   - bits  0..3   = vertex size / 4 (vertexSize = (desc<<2) & 0x3C)
	//   - bits  4N+4..4N+7 = attribute N's byte offset / 4
	//   - bits 44..53  = stream-0 presence flags (VF_VERTEX | VF_UV | ...)
	//   - bits 54..63  = stream-1 presence flags (unused for static TriShape)
	//
	// VF_TANGENT (bit 4) is REQUIRED for any mesh that hits the non-landscape
	// PBR shader path.  TruePBR routes us there because our material is
	// BSLightingShaderMaterialPBR with GetFeature()==kDefault and our property
	// has kVertexLighting set, and the standard PBR lighting VS reads
	// `float4 Bitangent: BINORMAL0` (Lighting.hlsl:48).  Without VF_TANGENT,
	// the IA layout has no BINORMAL0 binding → shader gets default (0,0,0,1)
	// → TBN matrix degenerates → normal mapping outputs garbage (the grey
	// shading symptom that prompted this fix).  For a flat horizontal grid
	// the tangent is trivially (1,0,0) at every vertex; we write it at +24
	// and shift COLOR to +28.
	//
	// Layout fields:
	//   size/4 = 8  → 32 bytes
	//   POSITION  offset/4 = 0  → byte 0
	//   TEXCOORD0 offset/4 = 4  → byte 16
	//   NORMAL    offset/4 = 5  → byte 20
	//   BINORMAL  offset/4 = 6  → byte 24  (tangent slot — name is historical)
	//   COLOR     offset/4 = 7  → byte 28
	//   Flags 0x3B = VF_VERTEX | VF_UV | VF_NORMAL | VF_TANGENT | VF_COLORS
	constexpr uint64_t kSnowVertexDesc =
		uint64_t{ 8 }           |   // size nibble (32 bytes / 4)
		(uint64_t{ 0 } << 4)    |   // VA_POSITION  offset / 4
		(uint64_t{ 4 } << 8)    |   // VA_TEXCOORD0 offset / 4
		(uint64_t{ 5 } << 16)   |   // VA_NORMAL    offset / 4
		(uint64_t{ 6 } << 20)   |   // VA_BINORMAL  offset / 4  (tangent storage)
		(uint64_t{ 7 } << 24)   |   // VA_COLOR     offset / 4
		(uint64_t{ 0x3B } << 44);   // flags: + VF_TANGENT

	// Compile-time sanity check using CommonLibSSE-NG's own decoder math.
	consteval uint32_t DecodeAttributeOffset(uint64_t desc, uint32_t attr)
	{
		return (desc >> (4 * attr + 2)) & 0x3C;
	}
	consteval uint32_t DecodeVertexSize(uint64_t desc)
	{
		return (desc << 2) & 0x3C;
	}
	static_assert(DecodeVertexSize(kSnowVertexDesc)        == LAND_VERTEX_STRIDE);  // 32
	static_assert(DecodeAttributeOffset(kSnowVertexDesc, 0) == 0);                  // pos
	static_assert(DecodeAttributeOffset(kSnowVertexDesc, 1) == 16);                 // uv
	static_assert(DecodeAttributeOffset(kSnowVertexDesc, 3) == 20);                 // normal
	static_assert(DecodeAttributeOffset(kSnowVertexDesc, 4) == 24);                 // binormal (tangent)
	static_assert(DecodeAttributeOffset(kSnowVertexDesc, 5) == 28);                 // color
	static_assert(((kSnowVertexDesc >> 44) & 0xFFFF) == 0x3B);                      // flags

	// Build the index buffer once at static-init.  Verified against the RE
	// session's gen_land_indices.py output:
	//   first 12 = [18,17,0, 0,1,18, 18,1,2, 2,19,18]
	//   last  12 = [286,269,270, 270,287,286, 288,287,270, 270,271,288]
	consteval std::array<uint16_t, LAND_INDICES> MakeSnowIndices()
	{
		std::array<uint16_t, LAND_INDICES> out{};
		int n = 0;
		for (int row = 0; row < LAND_QUADS_PER_SIDE; ++row) {
			for (int col = 0; col < LAND_QUADS_PER_SIDE; ++col) {
				const uint16_t BL = static_cast<uint16_t>(row * LAND_VERTS_PER_SIDE + col);
				const uint16_t BR = static_cast<uint16_t>(BL + 1);
				const uint16_t TL = static_cast<uint16_t>(BL + LAND_VERTS_PER_SIDE);
				const uint16_t TR = static_cast<uint16_t>(TL + 1);
				if (((col + row) & 1) == 0) {
					out[n++] = TR; out[n++] = TL; out[n++] = BL;
					out[n++] = BL; out[n++] = BR; out[n++] = TR;
				} else {
					out[n++] = TL; out[n++] = BL; out[n++] = BR;
					out[n++] = BR; out[n++] = TR; out[n++] = TL;
				}
			}
		}
		return out;
	}
	constexpr auto kSnowIndices = MakeSnowIndices();
	// First quad (col=0, row=0) checkerboard parity 0 → triangle (TR, TL, BL).
	// TR = LAND_VERTS_PER_SIDE + 1, TL = LAND_VERTS_PER_SIDE, BL = 0.
	static_assert(kSnowIndices[0] == LAND_VERTS_PER_SIDE + 1);
	static_assert(kSnowIndices[1] == LAND_VERTS_PER_SIDE);
	static_assert(kSnowIndices[2] == 0);
	static_assert(kSnowIndices[3] == 0);
	static_assert(kSnowIndices[4] == 1);
	static_assert(kSnowIndices[5] == LAND_VERTS_PER_SIDE + 1);
	// Last index is in range — sanity check on total IB size.
	static_assert(kSnowIndices[LAND_INDICES - 1] < LAND_VERTS);

	// Writes one vertex into a 32-byte slot (PBR-capable land vertex layout —
	// like the default 28-byte landscape format plus a 4-byte tangent slot
	// inserted between normal and color).
	//   pos[4]    @ 0   16 bytes  — float4 (x, y, z, w=1.0)
	//   uv[2]     @ 16   4 bytes  — int16 normalized [0, 32767]
	//   normal[2] @ 20   4 bytes  — int16 snorm (xy of unit normal; shader recovers z)
	//   tan[2]    @ 24   4 bytes  — int16 snorm (xy of unit tangent; shader recovers z)
	//   color[4]  @ 28   4 bytes  — uint8 RGBA
	inline void PackSnowVertex(uint8_t* dst,
		float x, float y, float z,
		float nx, float ny, float /*nz*/,
		float u, float v,
		uint8_t color)
	{
		// Position (full float4)
		const float pos[4] = { x, y, z, 1.0f };
		std::memcpy(dst + 0, pos, sizeof(pos));

		// UV — unsigned-normalized int16
		const auto to_i16_unorm = [](float t) -> int16_t {
			const float c = std::clamp(t, 0.0f, 1.0f) * 32767.0f;
			return static_cast<int16_t>(c + (c >= 0.0f ? 0.5f : -0.5f));
		};
		const int16_t uv[2] = { to_i16_unorm(u), to_i16_unorm(v) };
		std::memcpy(dst + 16, uv, sizeof(uv));

		// Normal xy — signed-normalized int16. Engine's snow/terrain pixel
		// shader recovers z = sqrt(1 - x² - y²) from sign-bit conventions.
		// Flat upward normal (0, 0, 1) → (0, 0) — trivial encoding.
		const auto to_i16_snorm = [](float t) -> int16_t {
			const float c = std::clamp(t, -1.0f, 1.0f) * 32767.0f;
			return static_cast<int16_t>(c + (c >= 0.0f ? 0.5f : -0.5f));
		};
		const int16_t normal[2] = { to_i16_snorm(nx), to_i16_snorm(ny) };
		std::memcpy(dst + 20, normal, sizeof(normal));

		// Tangent xy — signed-normalized int16, same packing as normal.  For a
		// flat horizontal grid (normal = +Z), the tangent points along world X
		// at every vertex.  Encoded (1, 0) → shader recovers z=0 → tangent
		// (1, 0, 0) → bitangent computed in shader = (0, 1, 0).  Result: TBN
		// matrix is well-formed, normal-map sampling produces correct world-
		// space normals, PBR lighting renders correctly.
		//
		// Without this slot the IA layout has no BINORMAL0 binding for the
		// standard PBR lighting VS — `float4 Bitangent: BINORMAL0` defaults
		// to (0,0,0,1), TBN collapses, normal mapping outputs garbage (the
		// grey shading we saw before this fix landed).
		const int16_t tangent[2] = { to_i16_snorm(1.0f), to_i16_snorm(0.0f) };
		std::memcpy(dst + 24, tangent, sizeof(tangent));

		// Color RGBA — grayscale input broadcast to RGB, alpha forced 0xFF.
		dst[28] = color;
		dst[29] = color;
		dst[30] = color;
		dst[31] = 0xFFu;
	}

	// Fills a LAND_VERTS-count CPU buffer (32 bytes per vert) with a flat
	// placeholder slab at local Z = 0.  Real per-vertex Z + mask culling lands
	// later via SnowDeformation::UpdateSlabVerticesIfNeeded once SceneHeight's
	// CPU mirrors finish their readback.  Until that update fires, the slab's
	// triangleCount is 0 (invisible) — so initial Z values are not rendered.
	//
	// Previous design read per-vertex Z from a `float[LAND_VERTS_PER_QUAD]`
	// passed in by the caller, but that constant was decoupled from the new
	// (post-density-bump) LAND_VERTS — caused 33×33 reads against a 17×17
	// array and crashed in Iteration 4.  Cleanest fix: drop the parameter
	// entirely.  The engine's per-cell-quad terrain heights are 17×17 anyway
	// and we'd have to interpolate to fill 33×33; SceneHeight's CPU mirror
	// is the authoritative source going forward.
	void PackSnowVertices(uint8_t* a_dst)
	{
		for (int row = 0; row < LAND_VERTS_PER_SIDE; ++row) {
			for (int col = 0; col < LAND_VERTS_PER_SIDE; ++col) {
				const int idx = row * LAND_VERTS_PER_SIDE + col;
				PackSnowVertex(a_dst + idx * LAND_VERTEX_STRIDE,
					col * LAND_VERT_SPACING,
					row * LAND_VERT_SPACING,
					0.0f,  // placeholder Z; real value from SceneHeight CPU mirror
					0.0f, 0.0f, 1.0f,
					static_cast<float>(col) / LAND_QUADS_PER_SIDE,
					static_cast<float>(row) / LAND_QUADS_PER_SIDE,
					0xFFu);
			}
		}
	}
}

// =============================================================================
// LoadEngineTexture — wrapper over BSShaderManager::GetTexture (AE 0x141480030)
//
// The engine's canonical texture loader.  Decompiled via Ghidra against AE 1170:
// signature is `void GetTexture(char* path, bool useFullLoader,
// NiPointer<NiTexture>* out, bool useErrFallback)`.  Internally it:
//   1) Normalizes the path with FUN_140d094f0 (strips/canonicalizes "textures\\"
//      and "Data\\Textures\\" prefixes against an internal cache key).
//   2) Resolves the cache key via FUN_140d0f4c0 (string → BSResource::ID).
//   3) Looks up an existing entry in the texture cache (FUN_140e0f6c0).  If
//      present, returns the cached NiSourceTexture (refcount-shared).
//   4) On miss, reads the DDS through BSResource::Stream + decodes via the
//      D3D11 device, allocates BSGraphics::Texture (0x28 bytes — ID3D11Resource*
//      + UAV + SRV + refcount), constructs NiSourceTexture (0x58 bytes) wrapping
//      it, inserts into the cache.
//   5) On failure (file missing, alloc failure), substitutes the engine's
//      default placeholder texture (DAT_14328ccb8 / DAT_14328cca0 — visible as
//      defaultTextureNormalMap / defaultTextureWhite in graphicsState).
//
// Net effect: caller gets a NiPointer to either the real texture or a default,
// never nullptr for a well-formed call.  Bit-identical to anything the engine
// loads itself during scene processing — feeds straight into the standard
// lighting shader's SetupMaterial pipeline.
// =============================================================================
RE::NiPointer<RE::NiSourceTexture> SnowDeformation::LoadEngineTexture(const char* a_dataRelativePath)
{
	if (!REL::Module::IsAE())
		return nullptr;
	if (!a_dataRelativePath || !*a_dataRelativePath)
		return nullptr;

	// Build "Data\\<relative>" — matches BSTextureSet::SetTexture (AE 0x1403272b0).
	// The relative parameter is rooted at the Textures\ folder by convention, so
	// callers pass e.g. "Textures\\landscape\\snow\\snow01.dds" and we prepend
	// just "Data\\" for the full filesystem-style path the engine expects.
	char fullPath[260];
	const int written = std::snprintf(fullPath, sizeof(fullPath), "Data\\%s", a_dataRelativePath);
	if (written < 0 || written >= static_cast<int>(sizeof(fullPath))) {
		logger::warn("[SnowDeformation] LoadEngineTexture: path too long: {}", a_dataRelativePath);
		return nullptr;
	}

	// Engine entry point.  useFullLoader=true → primary DDS loader with cache;
	// false → simpler alt loader (FUN_140e10f60), no fallback to default texture.
	// useErrFallback=false → on miss, return the "default" placeholder rather
	// than the more aggressive "ERR" placeholder (DAT_14328cca0).
	using BSShaderManager_GetTexture_t =
		void (*)(const char*, bool, RE::NiPointer<RE::NiTexture>*, bool);
	static const REL::Relocation<BSShaderManager_GetTexture_t>
		GetTexture{ REL::Offset(0x1480030) };

	RE::NiPointer<RE::NiTexture> result;
	GetTexture(fullPath, /*useFullLoader=*/true, &result, /*useErrFallback=*/false);

	// Engine always writes something into result for a well-formed call; nullptr
	// would mean an extreme allocator failure deep in the loader.  Be defensive.
	if (!result)
		return nullptr;

	// Downcast NiTexture → NiSourceTexture.  GetTexture always returns
	// NiSourceTexture-derived instances when invoked through the DDS path; we
	// never see NiRenderedTexture / NiPersistentSrcTextureRendererData here.
	return RE::NiPointer<RE::NiSourceTexture>(
		static_cast<RE::NiSourceTexture*>(result.get()));
}

// =============================================================================
// EnsureSnowTexturesLoaded
//
// Lazy-load the snow PBR texture set once per session (first BuildSnowQuadMesh
// call).  Path defaults aim at vanilla SSE snow ground assets — if the user has
// a PBR snow texture pack installed (Snow Cover's default texture pack, a third
// -party PBR snow mod), those paths typically override the vanilla ones in the
// game's loose-files-over-BSA precedence, so we automatically get PBR-baked
// textures without any per-mod configuration.
//
// Each texture is acquired through the engine's refcount-shared cache, so all
// snow slabs across all cells share a single set of GPU resources — building a
// new cell-quad costs only the NiPointer ref-bump (~one atomic increment), not
// a fresh DDS upload.
// =============================================================================
void SnowDeformation::EnsureSnowTexturesLoaded()
{
	if (snowDiffuseTexture && snowNormalTexture && snowRmaosTexture)
		return;  // fully loaded, fast-path out

	// Faultier's PBR Snow texture pack convention — `Textures\PBR\Landscape\`
	// is the standard install location for community PBR landscape textures
	// (referenced in the True PBR wiki as the recommended modders resource).
	// Plain vanilla `Textures\landscape\snow\` doesn't ship PBR-baked rmaos,
	// so we go straight to the PBR path.  If the user's install doesn't have
	// the pack, the engine substitutes default textures (logged below) and
	// the slab renders flat-shaded snow that still respects lighting/shadows.
	if (!snowDiffuseTexture)
		snowDiffuseTexture = LoadEngineTexture("Textures\\PBR\\landscape\\snow01.dds");
	if (!snowNormalTexture)
		snowNormalTexture = LoadEngineTexture("Textures\\PBR\\landscape\\snow01_n.dds");
	if (!snowRmaosTexture)
		snowRmaosTexture = LoadEngineTexture("Textures\\PBR\\landscape\\snow01_rmaos.dds");

	// Diagnostic: classify each result against the engine's default fallback
	// textures (graphicsState.defaultTextureNormalMap / defaultTextureWhite /
	// defaultTextureBlack).  If `BSShaderManager::GetTexture` couldn't find the
	// file on disk or in any loaded BSA, it returns one of these defaults
	// rather than nullptr — so a non-null NiPointer doesn't guarantee a real
	// load happened.  Comparing pointer identity is the canonical "did this
	// actually load?" check, used by TruePBR itself (see TruePBR.cpp:947 etc.).
	const auto& stateData = globals::game::graphicsState->GetRuntimeData();
	const auto* defNormal = stateData.defaultTextureNormalMap.get();
	const auto* defWhite  = stateData.defaultTextureWhite.get();
	const auto* defBlack  = stateData.defaultTextureBlack.get();

	auto classify = [&](const RE::NiPointer<RE::NiSourceTexture>& tex) -> const char* {
		if (!tex)                                                  return "NULL";
		if (tex.get() == defNormal)                                return "DEFAULT_NORMAL (file not found)";
		if (tex.get() == defWhite)                                 return "DEFAULT_WHITE  (file not found)";
		if (tex.get() == defBlack)                                 return "DEFAULT_BLACK  (file not found)";
		return "OK (loaded from disk/BSA)";
	};

	logger::info("[SnowDeformation] snow PBR textures:");
	logger::info("[SnowDeformation]   diffuse @ {}  status: {}",
		static_cast<void*>(snowDiffuseTexture.get()), classify(snowDiffuseTexture));
	logger::info("[SnowDeformation]   normal  @ {}  status: {}",
		static_cast<void*>(snowNormalTexture.get()),  classify(snowNormalTexture));
	logger::info("[SnowDeformation]   rmaos   @ {}  status: {}",
		static_cast<void*>(snowRmaosTexture.get()),   classify(snowRmaosTexture));
}

// =============================================================================
// BuildSnowQuadMesh — M2 procedural snow BSTriShape construction (PBR-shaded).
//
// Build a flat 17×17 grid snow BSTriShape per cell-quad and return it ready for
// scene-graph attachment.  Geometry follows the heightmap directly — no cutting,
// no displacement, no tessellation.  Those are M3/M4 work (accumulation map +
// fragmenting + GPU tessellation displacement).
//
// All engine factory addresses confirmed via Ghidra MCP against the AE 1170
// binary (May 2026).  Struct offsets verified against Ghidra's data-type
// layouts and Nukem9's SE RE — they agree.  AE-only during development; SE
// addresses are a mechanical follow-up (address-library ID lookup) before
// release.
//
// Construction sequence:
//   1) Renderer::CreateTriShape (AE @ 0x140E45F70)
//       → BSGraphics::TriShape (0x30 bytes) with D3D11 VB+IB allocated under
//         D3D11_USAGE_DEFAULT, raw vertex/index CPU staging buffers populated
//         (memcpy'd from our PackSnowVertices output), refCount=1, vertexDesc
//         set.  The engine owns the entire lifecycle — destructor frees via
//         Renderer vtable[5] = FreeRendererData.
//   2) BSTriShape::Create (AE @ 0x140D2D730)
//       → BSTriShape (0x160 bytes) — parent BSGeometry chain initialized
//         (vtable, name, transforms, flags), VTABLE_BSTriShape installed,
//         ucType = GEOMETRY_TYPE_TRISHAPE (3), triangle/vertex counts zeroed.
//   3) Wire BSGraphics::TriShape onto BSGeometry::pRendererData (+0x138),
//      duplicate VertexDesc onto BSGeometry::uiVertexDesc (+0x148), set the
//      triangle/vertex counts, set a conservative fixed model bound (slab
//      vertex Z gets fully recomputed later by UpdateSlabVerticesIfNeeded).
//   4) BSLightingShaderProperty: MemoryManager::Allocate + in-place Ctor
//      (AE @ 0x1414ACC20).  Ctor sets default flags kOwnEmit, kZBufferTest,
//      kZBufferWrite.  We add kCastShadows + kReceiveShadows + kVertexLighting
//      after (kVertexLighting is the PBR-path signal — see step 6).
//   5) prop->SetMaterial(BSLightingShaderMaterialPBR stack-temp, true) — engine
//      clones via Create()+CopyMembers into a heap canonical that prop->material
//      now owns.  Stack temp dies at scope end, All.erase tidies its map entry.
//   6) Patch the resulting PBR material in-place: engine-loaded snow diffuse +
//      normal (via BSShaderManager::GetTexture → cached NiPointers shared across
//      all slabs), engine-default rmaos/emissive/displacement/features for the
//      PBR-specific slots if no PBR snow pack is installed, snow-shaping
//      (Subsurface flag, roughness/spec scalars).  Property kVertexLighting +
//      material GetFeature==kDefault is the dual signal that flips the standard
//      lighting shader into the PBR rendering path.
//   7) NiPointer<BSShaderProperty>(prop) onto BSGeometry::shaderProperty
//      (+0x128).  NiPointer ctor AddRefs prop (0→1).  Property stays alive
//      until parent BSTriShape dies; on cell unload the chain unwinds and
//      everything is freed in the right order.
//
// Destruction (engine-handled when refcount drops to 0):
//   BSTriShape::~BSTriShape walks vtable[5] on the Renderer singleton to free
//   rendererData (D3D11 buffers + raw arrays + BSGraphics::TriShape struct
//   itself), then chains to ~BSGeometry which drops the spProperties
//   NiPointers → property dtor → material dtor.
// =============================================================================
RE::NiPointer<RE::BSTriShape> SnowDeformation::BuildSnowQuadMesh(RE::BSTriShape* a_terrainGeom)
{
	// AE-only during development.  The REL::Offset addresses below are AE 1170
	// RVAs; resolving them on an SE binary would land on garbage and crash.
	// Bail early on non-AE runtimes — SE addresses are a mechanical follow-up.
	if (!REL::Module::IsAE()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			logger::warn("[SnowDeformation] BuildSnowQuadMesh: AE-only during development; SE/VR pending");
		}
		return nullptr;
	}

	auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto* memmgr   = RE::MemoryManager::GetSingleton();
	if (!renderer || !memmgr)
		return nullptr;

	// ---- Load engine-native snow PBR textures (cached after first build).
	// Bit-identical to anything the engine loads through standard scene processing
	// — `BSShaderManager::GetTexture` allocates the full BSGraphics::Texture +
	// D3D11 resource + SRV chain and refcount-shares across all consumers.
	//
	// M1's borrow-from-terrain pattern is gone: it (a) was always wrong (terrain
	// textures aren't snow textures), and (b) is now obsolete because the
	// engine's loader gives us a real NiSourceTexture without any local D3D11
	// glue.  Engine substitutes a default texture on file-miss (no crash path),
	// so we don't need a defensive borrow fallback.
	//
	// `a_terrainGeom` is now retained only as a context handle for the caller's
	// scene-graph attachment phase — the M3 displacement work will read its
	// VertexDesc/UV layout for shared-vertex deformation correspondence.
	(void)a_terrainGeom;
	EnsureSnowTexturesLoaded();

	if (!snowDiffuseTexture || !snowNormalTexture) {
		// Catastrophic — engine loader gave up entirely.  Don't try to render
		// without textures; BSLightingShader::SetupMaterial dereferences these.
		logger::warn("[SnowDeformation] engine snow textures unavailable — no mesh built");
		return nullptr;
	}

	// ---- 1) Pack the 32-byte vertex stream into a stack buffer.  CreateTriShape
	//         memcpys this into its own MemoryManager-allocated shadow at
	//         rd->rawVertexData and uploads to the GPU VB. ----
	constexpr size_t kVertexBytes = LAND_VERTS * LAND_VERTEX_STRIDE;  // 289 × 32 = 9248
	std::array<uint8_t, kVertexBytes> vbuf{};
	PackSnowVertices(vbuf.data());

	// ---- 2) Renderer::CreateTriShape (AE 0x140E45F70) — engine factory.
	//         Allocates BSGraphics::TriShape, the D3D11 VB+IB (USAGE_DEFAULT),
	//         the raw CPU staging shadows, memcpys our data in, calls
	//         ID3D11Device::CreateBuffer twice.  Engine owns destruction. ----
	using Renderer_CreateTriShape_t = RE::BSGraphics::TriShape* (*)(
		RE::BSGraphics::Renderer*,
		void*    /*vertexData*/,
		uint32_t /*vertexBytes*/,
		uint64_t /*vertexDesc*/,
		void*    /*indexData*/,
		uint32_t /*indexCount*/);
	static const REL::Relocation<Renderer_CreateTriShape_t>
		Renderer_CreateTriShape{ REL::Offset(0xE45F70) };

	auto* rd = Renderer_CreateTriShape(
		renderer,
		vbuf.data(), static_cast<uint32_t>(kVertexBytes),
		kSnowVertexDesc,
		reinterpret_cast<void*>(const_cast<uint16_t*>(kSnowIndices.data())),
		static_cast<uint32_t>(LAND_INDICES));
	if (!rd)
		return nullptr;

	// ---- 3) BSTriShape::Create (AE 0x140D2D730) — engine factory.
	//         Allocates 0x160 bytes via MemoryManager, calls BSGeometry::Ctor
	//         (parent), installs VTABLE_BSTriShape, sets ucType=3 (TRISHAPE),
	//         zeros triangle/vertex counts. ----
	using BSTriShape_Create_t = RE::BSTriShape* (*)();
	static const REL::Relocation<BSTriShape_Create_t>
		BSTriShape_Create{ REL::Offset(0xD2D730) };

	auto* tri = BSTriShape_Create();
	if (!tri) {
		// rd leaks here — Renderer::FreeRendererData (vtable[5]) is the
		// symmetric cleanup but we don't have a clean wrapper for it yet.
		// Allocation failure under memory pressure is rare; revisit if it surfaces.
		logger::warn("[SnowDeformation] BSTriShape::Create returned null; leaking BSGraphics::TriShape at {}",
			static_cast<void*>(rd));
		return nullptr;
	}

	// ---- 4) Wire BSGraphics::TriShape onto BSGeometry; populate counts. ----
	auto& geomData = tri->GetGeometryRuntimeData();
	geomData.rendererData = rd;
	std::memcpy(&geomData.vertexDesc, &kSnowVertexDesc, sizeof(uint64_t));

	auto& triData         = tri->GetTrishapeRuntimeData();
	triData.vertexCount   = static_cast<uint16_t>(LAND_VERTS);
	triData.triangleCount = static_cast<uint16_t>(LAND_INDICES / 3);

	// ---- 5) Model bound — engine frustum culling. ----
	//   Conservative fixed-radius bound covering the 2048×2048 footprint plus
	//   ample vertical range to encompass any heightmap-driven vertex Z that
	//   UpdateSlabVerticesIfNeeded might write later (building rooftops can
	//   sit hundreds of units above the slab's local origin; DEAD_Z verts go
	//   far below).  Engine never re-computes modelBound after construction,
	//   so the bound MUST cover all possible future vertex positions or the
	//   slab gets frustum-culled when it shouldn't be.
	//
	//   5000-unit radius: 2048/√2 ≈ 1448 horizontal + 4500 vertical buffer.
	//   Cost of an overly-generous bound is just slightly fewer frustum-culls
	//   — performance, not correctness.
	auto& modelData = tri->GetModelData();
	modelData.modelBound.center = RE::NiPoint3{
		LAND_QUAD_SIZE * 0.5f,
		LAND_QUAD_SIZE * 0.5f,
		0.0f
	};
	modelData.modelBound.radius = 5000.0f;

	// ---- 6) BSLightingShaderProperty: allocate + in-place Ctor. ----
	auto* prop = static_cast<RE::BSLightingShaderProperty*>(
		memmgr->Allocate(sizeof(RE::BSLightingShaderProperty), 0, false));
	if (!prop) {
		// Property alloc failed.  Return the bare mesh — engine will render
		// it with whatever default property the geometry chain has.  Beats
		// leaking the geometry.
		return RE::NiPointer<RE::BSTriShape>(tri);
	}

	// Engine's in-place ctor (AE 0x1414ACC20):
	//   - Chains to BSShaderProperty parent ctor
	//   - Installs VTABLE_BSLightingShaderProperty
	//   - Initializes member defaults (alpha=0, flags=0, lists=NULL, …)
	//   - Calls SetMaterial(this, engineDefaultMaterial, false)
	//   - Allocates a 12-byte effectData block
	//   - Sets default flags: kOwnEmit, kZBufferTest, kZBufferWrite
	using BSLightingShaderProperty_Ctor_t =
		RE::BSShaderProperty* (*)(RE::BSLightingShaderProperty*);
	static const REL::Relocation<BSLightingShaderProperty_Ctor_t>
		BSLightingShaderProperty_Ctor{ REL::Offset(0x14ACC20) };
	BSLightingShaderProperty_Ctor(prop);

	// ---- 7) PBR material with engine-loaded snow diffuse + normal.
	//
	//   We use BSLightingShaderMaterialPBR (non-landscape) because the snow slab is
	//   conceptually a static prop draped over terrain — single texture set, no
	//   6-tile blending.  The recipe follows TruePBR.cpp:1284-1317
	//   (BSTempEffectGeometryDecal_Initialize): construct a stack-temp source,
	//   SetMaterial(true) to let the engine clone it via Create()+CopyMembers
	//   into the canonical heap material, then patch textures on the result.
	//
	//   Why SetMaterial(true) over the M1 bypass pattern (SetMaterial(nullptr) then
	//   direct assign):
	//     - SetMaterial(true) handles vtable install, base-class init, and refcount
	//       wiring that the engine expects on a fresh property→material binding.
	//     - We have no pre-existing texture state to preserve, so dedup isn't a
	//       concern (M1 needed the bypass because textures were attached pre-call;
	//       here we attach textures post-call).
	//     - Stack-temp dtor runs at scope end → All.erase removes the stack-key
	//       entry; the heap canonical's entry (created during CopyMembers) survives.
	{
		BSLightingShaderMaterialPBR srcMaterial;
		prop->SetMaterial(&srcMaterial, true);
	}

	auto* pbrMat = static_cast<BSLightingShaderMaterialPBR*>(prop->material);

	// Engine-loaded snow PBR textures, refcount-shared via the engine's texture
	// cache.  Albedo + normal land on the standard BSLightingShaderMaterialBase
	// slots; rmaos / features go in PBR-specific slots.  When TruePBR's
	// BSLightingShader::SetupMaterial hook runs on our property, these are the
	// pointers it walks to bind PS texture registers t0/t1/t5/t7 (and friends).
	pbrMat->diffuseTexture = snowDiffuseTexture;
	pbrMat->normalTexture  = snowNormalTexture;

	// UV tile scale — covers vertex UV [0,1] over the 2048-unit quad with
	// `kSnowUVTileRepeats` repeats so the texture doesn't smear.  Both stage 0
	// and stage 1 set to the same value (matches TruePBR.cpp:629-630's pattern
	// for `texCoordScale[1] = texCoordScale[0]`).
	pbrMat->texCoordScale[0] = { kSnowUVTileRepeats, kSnowUVTileRepeats };
	pbrMat->texCoordScale[1] = { kSnowUVTileRepeats, kSnowUVTileRepeats };

	// PBR-specific texture slots.  rmaos prefers our engine-loaded variant when
	// the user has a PBR snow texture pack on disk; if not, the engine returned
	// its `defaultTextureWhite` (rough=1, metal=0, AO=1, F0=1) which is a
	// reasonable PBR-neutral and matches what `ReceiveValuesFromRootMaterial`
	// would have patched in anyway.  Seeding here keeps the material in a
	// fully-defined state from frame 0 and makes the textures legible in RenderDoc.
	//   - rmaos     → snowRmaosTexture, or engine default if file missing
	//   - emissive  → defaultBlack (no emission)
	//   - displ.    → defaultBlack (parallax disabled for M2; M3 wires displacement)
	//   - feat0/1   → defaultWhite (no SSS/coat/fuzz signal — Snow Cover blend
	//                  patches these in the shader's PBR path via ApplySnowPBR)
	const auto& stateData = globals::game::graphicsState->GetRuntimeData();
	pbrMat->rmaosTexture        = snowRmaosTexture ? snowRmaosTexture : stateData.defaultTextureWhite;
	pbrMat->emissiveTexture     = stateData.defaultTextureBlack;
	pbrMat->displacementTexture = stateData.defaultTextureBlack;
	pbrMat->featuresTexture0    = stateData.defaultTextureWhite;
	pbrMat->featuresTexture1    = stateData.defaultTextureWhite;

	// Snow-shaping defaults.  Subsurface for the lit-from-within snow look; standard
	// PBR roughness/spec.  Snow Cover's WorldSettings will eventually override these
	// at render time via a SetupMaterial hook (M2 follow-up).
	pbrMat->pbrFlags.set(PBRFlags::Subsurface);
	pbrMat->specularColorScale = 0.85f;  // roughness scale: slightly under rmaos.r=1.0
	pbrMat->specularPower      = 0.04f;  // nonmetal F0 — standard PBR dielectric default

	// Emissive defaults — snow doesn't glow on its own; match TruePBR.cpp:1304-1306
	// pattern (whiteColor + mult=0 means "no emission" while keeping the cbuffer
	// path well-defined).
	{
		constexpr RE::NiColor whiteColor(1.f, 1.f, 1.f);
		if (prop->emissiveColor)
			*prop->emissiveColor = whiteColor;
		prop->emissiveMult = 0.f;
	}

	// Rendering-pass flags.  Ctor pre-set kOwnEmit / kZBufferTest / kZBufferWrite.
	// kVertexLighting is REQUIRED now — paired with material->GetFeature()==kDefault
	// (BSLightingShaderMaterialPBR::GetFeature() returns kDefault by design), it's
	// the signal that flips the standard lighting shader into the PBR path.
	//   M1 had kVertexLighting OFF because the material was a plain kDefault base
	//   without PBR fields, so the PBR static_cast<MaterialPBR*> would have read
	//   garbage at rmaosTexture/etc.  M2 has a real PBR material, so kVertexLighting
	//   is now the correct signal.
	using F = RE::BSShaderProperty::EShaderPropertyFlag8;
	prop->SetFlags(F::kCastShadows,     true);
	prop->SetFlags(F::kReceiveShadows,  true);
	prop->SetFlags(F::kVertexLighting,  true);  // PBR signal

	// ---- 8) Attach property onto BSGeometry::spProperties[1] (offset 0x128).
	// NiPointer ctor AddRefs prop (0 → 1); the local prop pointer falls out of
	// scope at function return without releasing; the NiPointer held by
	// geomData keeps it alive until the parent BSTriShape dies.
	geomData.shaderProperty = RE::NiPointer<RE::BSShaderProperty>(prop);

	return RE::NiPointer<RE::BSTriShape>(tri);
}

// =============================================================================
// CompileShaders
//   M1: only the deformation CS.  No slab VS/PS — the slab is now a real
//   engine-rendered BSTriShape, no bespoke draw call.  M2 will add a separate
//   VB-update CS that takes the snow mesh's vertex buffer as UAV and writes
//   per-vertex Z from the deformation field.
// =============================================================================
void SnowDeformation::CompileShaders()
{
	if (auto* rawPtr = reinterpret_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformCS.hlsl", {}, "cs_5_0")))
		snowDeformCS.attach(rawPtr);
}

// =============================================================================
// SetupResources
// =============================================================================
void SnowDeformation::SetupResources()
{
	auto device = globals::d3d::device;

	// Deformation ping-pong textures (R16_FLOAT)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width            = GRID_DIM;
		texDesc.Height           = GRID_DIM;
		texDesc.MipLevels        = 1;
		texDesc.ArraySize        = 1;
		texDesc.Format           = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc       = { 1, 0 };
		texDesc.Usage            = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format                    = texDesc.Format;
		srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels       = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format             = texDesc.Format;
		uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		for (int i = 0; i < 2; i++) {
			texSnowDeform[i] = eastl::make_unique<Texture2D>(texDesc,
				i == 0 ? "SnowDeformation::DeformField0" : "SnowDeformation::DeformField1");
			texSnowDeform[i]->CreateSRV(srvDesc);
			texSnowDeform[i]->CreateUAV(uavDesc);
		}

		// D3D11 does not zero-initialise DEFAULT-usage textures; clear both ping-pong
		// buffers so the first CS frame reads 0 instead of undefined memory, which
		// would otherwise create spurious "ghost" deformation on frame 1.
		{
			auto context = globals::d3d::context;
			static const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearUnorderedAccessViewFloat(texSnowDeform[0]->uav.get(), kZero);
			context->ClearUnorderedAccessViewFloat(texSnowDeform[1]->uav.get(), kZero);
		}
	}

	// Ridge magnitude texture (R16_FLOAT, single — written and read each frame)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width            = GRID_DIM;
		texDesc.Height           = GRID_DIM;
		texDesc.MipLevels        = 1;
		texDesc.ArraySize        = 1;
		texDesc.Format           = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc       = { 1, 0 };
		texDesc.Usage            = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format                    = texDesc.Format;
		srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels       = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format             = texDesc.Format;
		uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		texSnowRidge = eastl::make_unique<Texture2D>(texDesc, "SnowDeformation::RidgeField");
		texSnowRidge->CreateSRV(srvDesc);
		texSnowRidge->CreateUAV(uavDesc);

		// Clear ridge to zero for the same reason as the deformation ping-pong buffers.
		{
			auto context = globals::d3d::context;
			static const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearUnorderedAccessViewFloat(texSnowRidge->uav.get(), kZero);
		}
	}

	// Terrain height texture (R32_FLOAT, CPU-uploaded, SRV-only for CS)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width      = GRID_DIM;
		texDesc.Height     = GRID_DIM;
		texDesc.MipLevels  = 1;
		texDesc.ArraySize  = 1;
		texDesc.Format     = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc = { 1, 0 };
		texDesc.Usage      = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format                    = texDesc.Format;
		srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels       = 1;

		texTerrainHeight = eastl::make_unique<Texture2D>(texDesc, "SnowDeformation::TerrainHeight");
		texTerrainHeight->CreateSRV(srvDesc);

		terrainHeightCPU.resize(GRID_DIM * GRID_DIM, 0.0f);
		terrainFillRow = 0;
		terrainInitialized = false;
	}

	// Bilinear WRAP sampler for DS / PS sampling of the deformation field
	{
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampDesc.MinLOD         = 0;
		sampDesc.MaxLOD         = D3D11_FLOAT32_MAX;

		winrt::com_ptr<ID3D11SamplerState> samp;
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, samp.put()));
		deformSampler = std::move(samp);
	}

	CompileShaders();
}

void SnowDeformation::ClearShaderCache()
{
	snowDeformCS = nullptr;
	CompileShaders();
}

// =============================================================================
// Prepass
//   Called each frame before scene rendering.  Updates the toroidal grid origin
//   from the current eye position (the only place grid-state mutation happens),
//   then incrementally fills terrain heights.
//   The actual CS dispatch happens later in DeformationPass() (called from
//   Deferred::DeferredPasses).
// =============================================================================
void SnowDeformation::Prepass()
{
	if (!loaded || !settings.Enable)
		return;

	// ---- Update grid origin from eye position ----
	// This is the single authoritative mutation of prevCellIDX/Y,
	// currentValidMargin*, currentArrayOrigin*, and currentGridWorldOrigin*.
	// GetCommonBufferData() is a pure read after this runs.
	auto eyePos = Util::GetEyePosition(0);

	int cellIDX = (int)round(eyePos.x / GRID_CELL_SIZE);
	int cellIDY = (int)round(eyePos.y / GRID_CELL_SIZE);

	if (prevCellIDX == INT_MAX) {
		currentValidMarginX = 0;
		currentValidMarginY = 0;
	} else {
		currentValidMarginX = prevCellIDX - cellIDX;
		currentValidMarginY = prevCellIDY - cellIDY;
	}

	prevCellIDX = cellIDX;
	prevCellIDY = cellIDY;

	// Toroidal array origin (positive modulo) — match GrassCollision's exact computation.
	currentArrayOriginX = ((cellIDX - (int)GRID_DIM / 2) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
	currentArrayOriginY = ((cellIDY - (int)GRID_DIM / 2) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

	// World-space origin of the grid minimum corner
	currentGridWorldOriginX = (cellIDX - (int)GRID_DIM / 2) * GRID_CELL_SIZE;
	currentGridWorldOriginY = (cellIDY - (int)GRID_DIM / 2) * GRID_CELL_SIZE;

	UpdateTerrainHeight();

	// Layer 3 Stage 1: re-update slab vertices when SceneHeight publishes a
	// new CPU mirror generation.  Cheap when nothing to do (compares cached
	// generations).
	UpdateSlabVerticesIfNeeded();
}

// =============================================================================
// RegisterSlab
//   Called from BuildSnowQuadMesh after a slab is built + attached.  Adds the
//   slab to `activeSlabs` so UpdateSlabVerticesIfNeeded can find it.  Sets
//   triangleCount=0 initially so the slab is invisible until the first vertex
//   update lands — avoids a one-frame artifact where the slab renders at
//   terrain-only Z before the heightmap-following Z lands.
// =============================================================================
void SnowDeformation::RegisterSlab(RE::BSTriShape* triShape, RE::NiNode* parent,
	float worldOriginX, float worldOriginY)
{
	if (!triShape || !parent)
		return;

	// IMPORTANT: caller (OnLandSetupMaterial / BuildSnowQuadMesh) already holds
	// `snowMeshMutex` for the entire cell-load cycle.  Acquiring it again here
	// is a recursive lock on std::mutex → undefined behavior, observed as a
	// std::system_error throw on MSVC (crash log 2026-05-18 04:55:02).
	// `RegisterSlab` runs entirely inside the caller's critical section by
	// contract; do not re-lock.

	// De-dup — the cell-quad hook can fire multiple times for the same quad
	// in edge cases.  Last-wins on parent (engine may re-parent on reload).
	for (auto& s : activeSlabs) {
		if (s.triShape.get() == triShape) {
			s.parent       = RE::NiPointer<RE::NiNode>(parent);
			s.worldOriginX = worldOriginX;
			s.worldOriginY = worldOriginY;
			return;
		}
	}

	ActiveSlab slab;
	slab.triShape      = RE::NiPointer<RE::BSTriShape>(triShape);
	slab.parent        = RE::NiPointer<RE::NiNode>(parent);
	slab.worldOriginX  = worldOriginX;
	slab.worldOriginY  = worldOriginY;
	slab.lastUpdateGen = 0;
	slab.everUpdated   = false;
	activeSlabs.push_back(std::move(slab));

	// Hide the slab until vertices get heightmap-corrected.  triangleCount=0
	// makes BSGeometry render zero primitives — invisible, no GPU cost.  Bumps
	// back to full count in UpdateSlabVerticesIfNeeded after the first update.
	triShape->GetTrishapeRuntimeData().triangleCount = 0;
}

// =============================================================================
// IsRegisteredSnowSlab
//   Render-thread query: "is this geometry one of OUR snow slabs?".  Used by
//   Skylighting's precipitation-occlusion hook to exclude our slabs from the
//   top-down depth render — preventing the feedback loop where slabs at Z=A
//   get captured by texOcclusion, read back as the "ground" by SnowMaskCS,
//   and used to push slabs to Z=A+layerDepth each frame.
//
// Thread safety: `snowMeshMutex` (mutable) is held briefly during the scan.
// Cell-load registration runs on a worker thread; this query runs on the
// render thread.  Mutex serialises them.  Activeslab count is tiny (~4-16),
// so linear scan + brief lock is well under one microsecond per call.
// =============================================================================
bool SnowDeformation::IsRegisteredSnowSlab(const RE::BSGeometry* geom) const noexcept
{
	if (!geom)
		return false;

	std::lock_guard lock(snowMeshMutex);
	for (const auto& s : activeSlabs) {
		// BSTriShape derives from BSGeometry — same address, safe comparison.
		if (static_cast<const RE::BSGeometry*>(s.triShape.get()) == geom)
			return true;
	}
	return false;
}

// =============================================================================
// UpdateSlabVerticesIfNeeded
//   For each active slab, if SceneHeight has a newer CPU mirror than what we
//   last applied to this slab: regenerate vertex Z from the CPU mirror,
//   marker-degenerate the vertices whose XY says no snow allowed, upload via
//   UpdateSubresource on the engine VB, mark the slab as updated.
//
//   First successful update flips triangleCount from 0 to its full count so
//   the engine starts rendering the slab.
// =============================================================================
void SnowDeformation::UpdateSlabVerticesIfNeeded()
{
	auto& sh = globals::features::sceneHeight;
	if (!sh.loaded || !sh.AreCPUMirrorsValid())
		return;

	const uint64_t currentGen = sh.GetCPUMirrorsGeneration();

	std::lock_guard lock(snowMeshMutex);

	// Prune stale entries first.  NiPointers keep both triShape and parent
	// alive at our refcount, so dereferencing is always safe.  Engine cell
	// unload calls DetachChild → slab leaves parent->children → we detect →
	// drop our refs → final engine refcount goes to 0 → slab dies.
	activeSlabs.erase(
		eastl::remove_if(activeSlabs.begin(), activeSlabs.end(),
			[](const ActiveSlab& s) {
				if (!s.triShape || !s.parent)
					return true;
				for (auto& childPtr : s.parent->GetChildren()) {
					if (childPtr.get() == s.triShape.get())
						return false;
				}
				return true;  // slab no longer parented here → engine detached
			}),
		activeSlabs.end());

	if (activeSlabs.empty())
		return;

	// SnowLayerDepth is the offset above the underlying surface where the snow
	// "top" sits.  We apply this once per vertex so the slab visibly rests on
	// top of buildings/terrain rather than coplanar with them.
	const float layerDepth = settings.SnowLayerDepth;

	// Vertex constants — defer to file-level LAND_* constants so density bumps
	// stay in one place.  The (anonymous-namespace) LAND_* values are visible
	// here because we're in the same TU.
	constexpr float DEAD_Z = -1.0e6f;  // sentinel Z for mask-blocked verts
	constexpr size_t kVertexBytesTotal = static_cast<size_t>(LAND_VERTS) * LAND_VERTEX_STRIDE;

	auto context = globals::d3d::context;
	if (!context)
		return;

	for (auto& slab : activeSlabs) {
		if (slab.lastUpdateGen >= currentGen)
			continue;
		if (!slab.triShape)  // shouldn't happen after the prune above, but defensive
			continue;

		auto& geomData = slab.triShape->GetGeometryRuntimeData();
		auto* rd       = geomData.rendererData;
		if (!rd)
			continue;

		// `rd->vertexBuffer` is the D3D11 VB.  We rebuild the entire (N+1)×(N+1)
		// vertex pack into a member buffer (sized once, reused per call so we
		// don't churn the heap), then UpdateSubresource it.  Member storage
		// avoids stack pressure as density grows (33×33 ≈ 34 KB, 65×65 ≈ 132 KB
		// — the latter would risk overflowing worker-thread stacks).
		if (slabVertexScratch.size() != kVertexBytesTotal)
			slabVertexScratch.assign(kVertexBytesTotal, uint8_t{ 0 });

		// Each vertex's UV scales linearly across the LAND_QUADS_PER_SIDE span;
		// normals stay flat (0, 0, 1); tangent stays world-X (1, 0, 0); color
		// stays 0xFF.  We re-emit these alongside Z so the full vertex is
		// rewritten — the engine VB's existing contents are overwritten cleanly.
		for (int row = 0; row < LAND_VERTS_PER_SIDE; ++row) {
			for (int col = 0; col < LAND_VERTS_PER_SIDE; ++col) {
				const int idx = row * LAND_VERTS_PER_SIDE + col;
				const float localX = col * LAND_VERT_SPACING;
				const float localY = row * LAND_VERT_SPACING;
				const float worldX = slab.worldOriginX + localX;
				const float worldY = slab.worldOriginY + localY;

				// Mask check first — short-circuit dead verts.
				const float maskVal = sh.SampleSnowMaskCPU(worldX, worldY);
				float       vertexZ;
				if (maskVal < 0.5f) {
					vertexZ = DEAD_Z;
				} else {
					// Scene-height projection.  Falls back to "vertex at slab's
					// own world Z" (i.e., local Z = layerDepth above current
					// slab origin) when the scene-height mirror has no data
					// at this XY.
					const float sceneZ = sh.SampleSceneHeightCPU(worldX, worldY);
					// vertex world Z target = sceneZ + layerDepth
					// vertex local Z = vertex world Z - slab world Z
					//   slab.world.translate.z already includes parent.world
					//   + slab.local — using it avoids any double-counting.
					const float slabWorldZ = slab.triShape->world.translate.z;
					if (sceneZ <= SceneHeight::kNoSceneHeightSentinel * 0.5f) {
						vertexZ = layerDepth;  // no scene data → snug above slab origin
					} else {
						vertexZ = (sceneZ + layerDepth) - slabWorldZ;
					}
				}

				// Pack into 32-byte layout (matches PackSnowVertex's layout in
				// the cell-load build path):
				//   pos float4 @ 0
				//   uv  int16x2 @ 16
				//   nrm int16x2 @ 20
				//   tan int16x2 @ 24
				//   col uint8x4 @ 28
				uint8_t* dst = slabVertexScratch.data() + idx * LAND_VERTEX_STRIDE;

				const float pos[4] = { localX, localY, vertexZ, 1.0f };
				std::memcpy(dst + 0, pos, sizeof(pos));

				const auto to_i16_unorm = [](float t) -> int16_t {
					const float c = std::clamp(t, 0.0f, 1.0f) * 32767.0f;
					return static_cast<int16_t>(c + (c >= 0.0f ? 0.5f : -0.5f));
				};
				const int16_t uv[2] = {
					to_i16_unorm(static_cast<float>(col) / static_cast<float>(LAND_QUADS_PER_SIDE)),
					to_i16_unorm(static_cast<float>(row) / static_cast<float>(LAND_QUADS_PER_SIDE))
				};
				std::memcpy(dst + 16, uv, sizeof(uv));

				// Flat upward normal (0, 0, 1) → encoded (0, 0).
				const int16_t nrm[2] = { 0, 0 };
				std::memcpy(dst + 20, nrm, sizeof(nrm));

				// Tangent (1, 0, 0) → encoded (32767, 0).
				const int16_t tan[2] = { 32767, 0 };
				std::memcpy(dst + 24, tan, sizeof(tan));

				dst[28] = 0xFF; dst[29] = 0xFF; dst[30] = 0xFF; dst[31] = 0xFF;
			}
		}

		// Push to GPU.  D3D11_USAGE_DEFAULT VB accepts UpdateSubresource —
		// matches how the engine itself updates dynamic terrain data.
		context->UpdateSubresource(
			reinterpret_cast<ID3D11Resource*>(rd->vertexBuffer),
			0, nullptr, slabVertexScratch.data(),
			static_cast<UINT>(kVertexBytesTotal), 0);

		// First successful update reveals the slab.  triangleCount matches the
		// index buffer (LAND_QUADS_PER_SIDE² × 2 triangles).  Mask-blocked
		// verts produce degenerate triangles via the DEAD_Z marker — GPU
		// culls those automatically without us touching the IB.
		auto& triData         = slab.triShape->GetTrishapeRuntimeData();
		triData.triangleCount = static_cast<uint16_t>(LAND_QUADS_PER_SIDE * LAND_QUADS_PER_SIDE * 2);

		slab.lastUpdateGen = currentGen;
		slab.everUpdated   = true;
	}
}

void SnowDeformation::DrawSettings()
{
	ImGui::Checkbox("Enable Snow Deformation", &settings.Enable);
	ImGui::SameLine();
	ImGui::TextDisabled("(M2: per-quad procedural BSTriShape on cell load, PBR-shaded — takes effect on next cell load)");

	ImGui::SliderFloat("Snow Layer Depth", &settings.SnowLayerDepth, 0.0f, 64.0f, "%.1f units");
	ImGui::SameLine();
	ImGui::TextDisabled("(how high the snow sits above the terrain — takes effect on next cell load)");

	ImGui::Checkbox("Build & Attach Mesh", &settings.BuildAndAttachMesh);
	ImGui::SameLine();
	ImGui::TextDisabled("(master switch — off skips builder + attach, hook still logs)");

	ImGui::TextDisabled("M2 status: builder calls Renderer::CreateTriShape + BSTriShape::Create + allocates BSLightingShaderProperty with BSLightingShaderMaterialPBR (kVertexLighting → PBR path). AE-only during development. Takes effect on next cell load.");

	ImGui::Separator();
	ImGui::TextDisabled("Milestone 3 (deformation) parameters — wired up but not yet read by the snow clones:");
	ImGui::SliderFloat("Contact Depth",         &settings.SnowContactDepth, 4.0f, 128.0f, "%.0f units");
	ImGui::SliderFloat("Settling Rate",         &settings.SettlingRate,     0.0f, 5.0f,   "%.3f /s");
	ImGui::SliderFloat("Ridge Strength",        &settings.RidgeStrength,    0.0f, 10.0f,  "%.2f");
	ImGui::SliderFloat("Snow Altitude Min",     &settings.SnowAltitudeMin,  -10000.0f, 50000.0f, "%.0f");
	ImGui::SliderFloat("Surface Epsilon",       &settings.TerrainSurfaceEpsilon, 0.0f, 32.0f, "%.1f units");
	ImGui::Checkbox  ("Debug: Force Deform",  &settings.DebugForceDeform);
}

void SnowDeformation::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SnowDeformation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SnowDeformation::RestoreDefaultSettings()
{
	settings = Settings{};
}

void SnowDeformation::PostPostLoad()
{
	Hooks::Install();
}

// =============================================================================
// Hooks::TESObjectLAND_SetupMaterial
//   Detours TESObjectLAND::SetupMaterial — fires once per cell-quad after the
//   engine has built terrain's per-cell LoadedLandData (heights, normals,
//   colors).  Same relocation TerrainHelper / TruePBR detour; Detours chains
//   them.  Order is install-order; we don't care which runs first as long as
//   the engine call completes before our handler reads LoadedLandData.
// =============================================================================
bool SnowDeformation::Hooks::TESObjectLAND_SetupMaterial::thunk(RE::TESObjectLAND* land)
{
	bool result = func(land);
	if (result) {
		auto& sd = globals::features::snowDeformation;
		if (sd.loaded) {
			sd.OnLandSetupMaterial(land);
		}
	}
	return result;
}

void SnowDeformation::Hooks::Install()
{
	stl::detour_thunk<TESObjectLAND_SetupMaterial>(REL::RelocationID(18368, 18791));
	logger::info("[SnowDeformation] Installed TESObjectLAND::SetupMaterial hook");
}
