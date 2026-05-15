#include "Globals.h"
#include "SnowDeformation.h"

#include "State.h"
#include "Utils/D3D.h"

#include "Deferred.h"
#include "Features/GrassCollision.h"
#include "Features/TerrainBlending.h"
#include "RE/B/BSLightingShaderMaterialLandscape.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowDeformation::Settings,
	Enable,
	SnowLayerDepth,
	SnowContactDepth,
	SettlingRate,
	RidgeStrength,
	TessellationScale,
	TessellationFalloff,
	SnowAltitudeMin,
	SnowSlopeFactor,
	TerrainSurfaceEpsilon,
	DebugForceDeform)

bool SnowDeformation::HasShaderDefine(RE::BSShader::Type shaderType)
{
	// Only BSLightingShader needs the define (PS writes masks.y terrain marker;
	// VS compiles with LANDSCAPE — same translation unit as PS).
	return shaderType == RE::BSShader::Type::Lighting;
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
	data.TessellationScale     = settings.TessellationScale;
	data.TessellationFalloff   = settings.TessellationFalloff;
	data.SnowAltitudeMin       = settings.SnowAltitudeMin;
	data.SnowSlopeFactor       = settings.SnowSlopeFactor;
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

	} else {
		// ---- Incremental update for newly-exposed cells after camera scroll ----
		int absMarginX = std::abs(currentValidMarginX);
		int absMarginY = std::abs(currentValidMarginY);

		// Large teleport: restart full fill
		if (absMarginX > (int)GRID_DIM / 2 || absMarginY > (int)GRID_DIM / 2) {
			terrainFillRow = 0;
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

	auto context  = globals::d3d::context;

	// Unbind DS SRVs from the geometry pass before using texSnowDeform as CS UAV.
	RestoreTessellationShaders();

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
// BindTessellationShaders
//   Binds snowHS + snowDS and their constant buffers.
//   Called from the DrawIndexed hook between base terrain and snow shell draws.
// =============================================================================
void SnowDeformation::BindTessellationShaders()
{
	if (!snowHS || !snowDS)
		return;

	auto context = globals::d3d::context;

	// HS/DS need b5 (SharedData) + b6 (FeatureData)
	ID3D11Buffer* sharedBufs[2] = {
		globals::state->sharedDataCB->CB(),
		globals::state->featureDataCB->CB()
	};
	context->HSSetConstantBuffers(5, 2, sharedBufs);
	context->DSSetConstantBuffers(5, 2, sharedBufs);

	// DS also needs b12 (PerFrame) for CameraViewProj + CameraPosAdjust
	ID3D11Buffer* perFrameBuf = *globals::game::perFrame;
	context->DSSetConstantBuffers(12, 1, &perFrameBuf);

	// DS reads the deformation field (t106) and ridge (t107)
	uint32_t readIdx = simFrameIndex % 2;
	ID3D11ShaderResourceView* dsSRVs[2] = {
		texSnowDeform[readIdx] ? texSnowDeform[readIdx]->srv.get() : nullptr,
		texSnowRidge            ? texSnowRidge->srv.get()           : nullptr
	};
	context->DSSetShaderResources(106, 2, dsSRVs);

	// Bilinear wrap sampler at s8
	if (deformSampler) {
		ID3D11SamplerState* samp = deformSampler.get();
		context->DSSetSamplers(8, 1, &samp);
	}

	context->HSSetShader(snowHS.get(), nullptr, 0);
	context->DSSetShader(snowDS.get(), nullptr, 0);

	// Switch the IA topology to 3-control-point patch list so the GPU activates HS+DS.
	// Called from the DrawIndexed hook between base terrain and snow shell draws.
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

	tessellationBound = true;
}

// =============================================================================
// RestoreTessellationShaders
//   Unbinds HS/DS and their SRVs, and resets topology to TRIANGLELIST.
//   Called at the start of each non-landscape SetupGeometry thunk, and from
//   DeformationPass() to clear DS SRVs before CS UAV binding.
// =============================================================================
void SnowDeformation::RestoreTessellationShaders()
{
	if (!tessellationBound)
		return;

	auto context = globals::d3d::context;

	// Clear DS SRVs so the deformation texture can be used as UAV in the CS
	ID3D11ShaderResourceView* nullSRVs[2] = {};
	context->DSSetShaderResources(106, 2, nullSRVs);

	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);

	// Restore topology to TRIANGLELIST for subsequent non-tessellated draws.
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	tessellationBound = false;
}

// =============================================================================
// CompileShaders
// =============================================================================
void SnowDeformation::CompileShaders()
{
	// Compute shader
	{
		if (auto* rawPtr = reinterpret_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowDeformCS.hlsl", {}, "cs_5_0")))
			snowDeformCS.attach(rawPtr);
	}

	// Hull shader (LANDSCAPE+VC+SNOW_DEFORMATION; HULLSHADER injected by CompileShader)
	{
		std::vector<std::pair<const char*, const char*>> defines = {
			{ "LANDSCAPE", "" },
			{ "VC", "" },
			{ "SNOW_DEFORMATION", "" }
		};
		if (auto* rawPtr = reinterpret_cast<ID3D11HullShader*>(
				Util::CompileShader(L"Data\\Shaders\\Lighting.hlsl", defines, "hs_5_0", "SnowHS_main")))
			snowHS.attach(rawPtr);
	}

	// Domain shader (same defines; DOMAINSHADER injected by CompileShader)
	{
		std::vector<std::pair<const char*, const char*>> defines = {
			{ "LANDSCAPE", "" },
			{ "VC", "" },
			{ "SNOW_DEFORMATION", "" }
		};
		if (auto* rawPtr = reinterpret_cast<ID3D11DomainShader*>(
				Util::CompileShader(L"Data\\Shaders\\Lighting.hlsl", defines, "ds_5_0", "SnowDS_main")))
			snowDS.attach(rawPtr);
	}

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

	// LESS_EQUAL DSS for the tessellated snow-shell draw in the DrawIndexed hook.
	// DepthWriteMask=ZERO: rely on the Z-prepass depth; don't overwrite with raised depth
	// (overwriting would incorrectly hide vegetation between physics terrain and raised snow).
	{
		D3D11_DEPTH_STENCIL_DESC dssDesc{};
		dssDesc.DepthEnable    = TRUE;
		dssDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dssDesc.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
		dssDesc.StencilEnable  = FALSE;
		winrt::com_ptr<ID3D11DepthStencilState> newDSS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&dssDesc, newDSS.put()));
		lessEqualDSS = std::move(newDSS);
	}

	CompileShaders();
}

void SnowDeformation::ClearShaderCache()
{
	snowDeformCS = nullptr;
	snowHS       = nullptr;
	snowDS       = nullptr;
	lessEqualDSS = nullptr;
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

	// Toroidal array origin (positive modulo)
	currentArrayOriginX = ((cellIDX - (int)GRID_DIM / 2) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
	currentArrayOriginY = ((cellIDY - (int)GRID_DIM / 2) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

	// World-space origin of the grid minimum corner
	currentGridWorldOriginX = (cellIDX - (int)GRID_DIM / 2) * GRID_CELL_SIZE;
	currentGridWorldOriginY = (cellIDY - (int)GRID_DIM / 2) * GRID_CELL_SIZE;

	UpdateTerrainHeight();
}

void SnowDeformation::DrawSettings()
{
	ImGui::Checkbox("Enable Snow Deformation", &settings.Enable);
	ImGui::SliderFloat("Snow Layer Depth", &settings.SnowLayerDepth, 0.0f, 32.0f, "%.1f units");
	ImGui::SameLine();
	ImGui::TextDisabled("(snow raise height = max compression depth)");
	ImGui::SliderFloat("Contact Depth", &settings.SnowContactDepth, 4.0f, 128.0f, "%.0f units");
	ImGui::SliderFloat("Settling Rate", &settings.SettlingRate, 0.0f, 5.0f, "%.3f /s");
	ImGui::SliderFloat("Ridge Strength", &settings.RidgeStrength, 0.0f, 10.0f, "%.2f");
	ImGui::Separator();
	ImGui::SliderFloat("Tessellation Scale", &settings.TessellationScale, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Tessellation Falloff", &settings.TessellationFalloff, 50.0f, 2000.0f, "%.0f units");
	ImGui::SliderFloat("Snow Altitude Min", &settings.SnowAltitudeMin, -10000.0f, 50000.0f, "%.0f");
	ImGui::SliderFloat("Surface Epsilon", &settings.TerrainSurfaceEpsilon, 0.0f, 32.0f, "%.1f units");
	ImGui::Checkbox("Debug: Force Deform", &settings.DebugForceDeform);
	ImGui::SameLine();
	ImGui::TextDisabled("(force fully-compressed state: terrain at physics level, no raise)");
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
// Hooks
// =============================================================================
void SnowDeformation::Hooks::BSLightingShader_SetupGeometry::thunk(
	RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& snow = globals::features::snowDeformation;

	// Safety: clear any stale pending flag from a previous pass that never drew
	snow.snowBatchPending = false;

	// If tessellation was bound for the previous draw, unbind it before proceeding.
	if (snow.tessellationBound)
		snow.RestoreTessellationShaders();

	// Detect landscape pass and mark it for the DrawIndexed hook.
	if (snow.loaded && snow.settings.Enable && snow.snowHS && snow.snowDS) {
		if (Pass && Pass->shaderProperty) {
			auto* material = Pass->shaderProperty->material;
			if (material &&
			    material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend)
			{
				// Only tessellate terrain that actually has snow texture layers.
				auto* landMat = static_cast<RE::BSLightingShaderMaterialLandscape*>(material);
				bool  hasSnow = false;
				for (int i = 0; i < 6; ++i) {
					if (landMat->textureIsSnow[i] > 0.5f) { hasSnow = true; break; }
				}
				if (hasSnow) {
					snow.snowBatchPending = true;
					// The DrawIndexed hook will issue two draws:
					//   1. base terrain at original position (no tessellation)
					//   2. raised snow shell with tessellation
				}
			}
		}
	}

	func(This, Pass, RenderFlags);
}

// =============================================================================
// DrawIndexed hook
//   When snowBatchPending is set by the SetupGeometry hook, this thunk handles
//   the double-draw: base terrain first (no tessellation), then the raised snow
//   shell with hull/domain shaders activated.
// =============================================================================
void STDMETHODCALLTYPE SnowDeformation::Hooks::ID3D11DeviceContext_DrawIndexed::thunk(
	ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	auto& snow = globals::features::snowDeformation;

	if (snow.snowBatchPending) {
		snow.snowBatchPending = false;

		// ── Draw 1: base terrain at original position (no tessellation) ──
		func(This, IndexCount, StartIndexLocation, BaseVertexLocation);

		// ── Draw 2: raised snow shell with tessellation ──
		auto context = globals::d3d::context;

		// Save the game's depth-stencil state so we can restore it after
		ID3D11DepthStencilState* originalDSS    = nullptr;
		UINT                     originalStencilRef = 0;
		context->OMGetDepthStencilState(&originalDSS, &originalStencilRef);

		// Bind HS/DS, deformation textures, PATCHLIST topology
		snow.BindTessellationShaders();

		// Override to LESS_EQUAL so raised-snow pixels (closer than the
		// Z-prepass + base-terrain draw) pass the depth test.
		context->OMSetDepthStencilState(snow.lessEqualDSS.get(), 0);

		func(This, IndexCount, StartIndexLocation, BaseVertexLocation);

		// Cleanup: restore DSS, unbind tessellation shaders, reset topology
		context->OMSetDepthStencilState(originalDSS, originalStencilRef);
		if (originalDSS)
			originalDSS->Release();

		snow.RestoreTessellationShaders();

		return;
	}

	func(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void SnowDeformation::Hooks::Install()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[SNOW DEFORMATION] Installed hooks");
}
