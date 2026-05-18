#include "SceneHeight.h"

#include "Deferred.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

// =============================================================================
// SetupResources
//   Allocates the water-mask texture + CPU shadow.  Called once at plugin init.
// =============================================================================
void SceneHeight::SetupResources()
{
	auto device = globals::d3d::device;

	// R32_FLOAT — full precision avoids float16 encoding hassle on the CPU side
	// (UpdateSubresource memcpys without conversion).  At 128x128 = 64 KB per
	// active window — trivial.  Could downsize to R16_FLOAT later if memory
	// pressure surfaces; the engine sentinel value -NI_INFINITY (~-FLT_MAX)
	// still survives the conversion safely below the -1e9 shader threshold.
	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width            = kResolution;
	texDesc.Height           = kResolution;
	texDesc.MipLevels        = 1;
	texDesc.ArraySize        = 1;
	texDesc.Format           = DXGI_FORMAT_R32_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage            = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
	texDesc.CPUAccessFlags   = 0;
	texDesc.MiscFlags        = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels       = 1;

	waterMaskTex = eastl::make_unique<Texture2D>(texDesc, "SceneHeight::WaterMask");
	waterMaskTex->CreateSRV(srvDesc);

	// CPU shadow — initialized to sentinel so any code path that samples before
	// the first refresh completes sees "no water" everywhere (graceful default).
	waterMaskCPU.assign(static_cast<size_t>(kResolution) * kResolution, kNoWaterSentinel);

	cachedCellX = INT_MIN;
	cachedCellY = INT_MIN;
	fillRow     = 0;
	ready       = false;

	// =========================================================================
	// Snow mask texture (Layer 2)
	// =========================================================================
	// R8_UNORM 128×128 — same world-space window as the water mask, single
	// byte per texel (0 = no snow, 1.0 = snow allowed; UNORM lets us use
	// linear filtering later if Layer 3 wants soft boundaries).  UAV-bound
	// so the SnowMaskCS can write to it; SRV-bound so consumers can sample.
	{
		D3D11_TEXTURE2D_DESC snowDesc{};
		snowDesc.Width            = kResolution;
		snowDesc.Height           = kResolution;
		snowDesc.MipLevels        = 1;
		snowDesc.ArraySize        = 1;
		snowDesc.Format           = DXGI_FORMAT_R8_UNORM;
		snowDesc.SampleDesc.Count = 1;
		snowDesc.Usage            = D3D11_USAGE_DEFAULT;
		snowDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		snowDesc.CPUAccessFlags   = 0;
		snowDesc.MiscFlags        = 0;

		D3D11_SHADER_RESOURCE_VIEW_DESC snowSRV{};
		snowSRV.Format                    = DXGI_FORMAT_R8_UNORM;
		snowSRV.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
		snowSRV.Texture2D.MostDetailedMip = 0;
		snowSRV.Texture2D.MipLevels       = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC snowUAV{};
		snowUAV.Format             = DXGI_FORMAT_R8_UNORM;
		snowUAV.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
		snowUAV.Texture2D.MipSlice = 0;

		snowMaskTex = eastl::make_unique<Texture2D>(snowDesc, "SceneHeight::SnowMask");
		snowMaskTex->CreateSRV(snowSRV);
		snowMaskTex->CreateUAV(snowUAV);
	}
	snowMaskValid = false;

	// =========================================================================
	// Scene height world-Z texture (Layer 2.5)
	// =========================================================================
	// R32_FLOAT 128×128 — same window + texel layout as the water/snow masks
	// but storing world-space Z values directly.  Written by SnowMaskCS,
	// readable from CPU after readback for slab vertex Z lookups.
	{
		D3D11_TEXTURE2D_DESC zDesc{};
		zDesc.Width            = kResolution;
		zDesc.Height           = kResolution;
		zDesc.MipLevels        = 1;
		zDesc.ArraySize        = 1;
		zDesc.Format           = DXGI_FORMAT_R32_FLOAT;
		zDesc.SampleDesc.Count = 1;
		zDesc.Usage            = D3D11_USAGE_DEFAULT;
		zDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		zDesc.CPUAccessFlags   = 0;
		zDesc.MiscFlags        = 0;

		D3D11_SHADER_RESOURCE_VIEW_DESC zSRV{};
		zSRV.Format                    = DXGI_FORMAT_R32_FLOAT;
		zSRV.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
		zSRV.Texture2D.MostDetailedMip = 0;
		zSRV.Texture2D.MipLevels       = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC zUAV{};
		zUAV.Format             = DXGI_FORMAT_R32_FLOAT;
		zUAV.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
		zUAV.Texture2D.MipSlice = 0;

		sceneHeightWorldTex = eastl::make_unique<Texture2D>(zDesc, "SceneHeight::SceneHeightWorld");
		sceneHeightWorldTex->CreateSRV(zSRV);
		sceneHeightWorldTex->CreateUAV(zUAV);
	}

	// =========================================================================
	// Linear-clamp sampler for the snow-mask CS
	// =========================================================================
	// Used by SnowMaskCS to sample texOcclusion at fractional UVs derived from
	// world XY projection.  Linear filtering smooths slope-derivative noise;
	// clamp address mode prevents wraparound at window edges (we explicitly
	// bounds-check in the shader but clamp is defensive).
	{
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxAnisotropy  = 1;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampDesc.MinLOD         = 0;
		sampDesc.MaxLOD         = D3D11_FLOAT32_MAX;

		ID3D11SamplerState* rawSampler = nullptr;
		if (SUCCEEDED(device->CreateSamplerState(&sampDesc, &rawSampler))) {
			linearClampSampler.attach(rawSampler);
		}
	}

	// =========================================================================
	// Compile SnowMaskCS
	// =========================================================================
	// Path matches the feature's shader install layout.  Same compile pattern
	// as SnowDeformation::CompileShaders.
	if (auto* rawCS = reinterpret_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\SceneHeight\\SnowMaskCS.hlsl", {}, "cs_5_0"))) {
		snowMaskCS.attach(rawCS);
	}

	// =========================================================================
	// CPU mirror staging textures (Layer 3 Stage 1)
	// =========================================================================
	// USAGE_STAGING + CPU_ACCESS_READ — same dims/format as their source
	// textures so CopyResource works as a direct one-shot.
	{
		D3D11_TEXTURE2D_DESC stagingDesc{};
		stagingDesc.Width            = kResolution;
		stagingDesc.Height           = kResolution;
		stagingDesc.MipLevels        = 1;
		stagingDesc.ArraySize        = 1;
		stagingDesc.Format           = DXGI_FORMAT_R8_UNORM;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.Usage            = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags        = 0;
		stagingDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags        = 0;

		snowMaskStaging = eastl::make_unique<Texture2D>(stagingDesc, "SceneHeight::SnowMaskStaging");
	}

	// Scene-height staging — R32_FLOAT 128×128, same dims/format as the
	// CS-output target (sceneHeightWorldTex).  Format-compatible CopyResource.
	{
		D3D11_TEXTURE2D_DESC stagingDesc{};
		stagingDesc.Width            = kResolution;
		stagingDesc.Height           = kResolution;
		stagingDesc.MipLevels        = 1;
		stagingDesc.ArraySize        = 1;
		stagingDesc.Format           = DXGI_FORMAT_R32_FLOAT;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.Usage            = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags        = 0;
		stagingDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags        = 0;

		sceneHeightStaging = eastl::make_unique<Texture2D>(stagingDesc, "SceneHeight::SceneHeightStaging");
	}

	snowMaskCPU.clear();
	sceneHeightCPU.clear();
	readbackPending      = false;
	cpuMirrorsValid      = false;
	cpuMirrorsGeneration = 0;
}

// =============================================================================
// Reset
//   Per-frame hook called from State::Reset (NOT a world-load hook — the name
//   is misleading).  Must NOT touch cell-change-detection state; doing so
//   would re-trigger RebaseToNewCell every frame and lock fillRow at 0 in an
//   infinite refresh loop.  Genuine cache invalidation goes through
//   InvalidateCache() below, called from the menu's "Force Refresh" button or
//   when a hook detects a true world-state change worth refreshing for.
// =============================================================================
void SceneHeight::Reset()
{
	// Intentionally empty.  Cell-change detection is the sole authority on
	// when the cache becomes stale.
}

// =============================================================================
// InvalidateCache
//   Explicit cache-invalidation entry point.  Forces the next Prepass to treat
//   the current cell as new, restarting the progressive fill from row 0.
//   Wired to the "Force Water Mask Refresh" menu button.  In the future, also
//   call from save-load / fast-travel hooks if water bodies can change without
//   crossing a cell boundary (currently they can't in vanilla Skyrim, so the
//   button is the only caller).
// =============================================================================
void SceneHeight::InvalidateCache()
{
	cachedCellX  = INT_MIN;
	cachedCellY  = INT_MIN;
	fillRow      = 0;
	ready        = false;
	snowMaskValid = false;
}

// =============================================================================
// Prepass
//   Drives the cell-change detection + progressive fill state machine.
//   1. If the player crossed a cell boundary, invalidate the cache.
//   2. While fill is in progress, populate kFillRowsPerFrame rows per frame.
//   3. When the fill completes, upload to GPU and flip `ready`.
//   4. Bind the SRV unconditionally so downstream shaders always have valid
//      data on the bound slot (sentinel-filled when not yet ready).
// =============================================================================
void SceneHeight::Prepass()
{
	if (!loaded)
		return;
	if (!waterMaskTex)
		return;

	// ---- 0) Skip during loading menus / main menu ----
	//   Cell streaming runs mid-transition; engine's TESObjectCELL data may be
	//   in an inconsistent state.  Calling TES::GetCell or TES::GetWaterHeight
	//   into that storm has crashed on loading screens (observed on AMD RX
	//   7900 series, but the underlying race is GPU-agnostic).
	//
	//   Skipping here means the cached cell stays valid until we're back in
	//   gameplay.  Once the load completes, DetectCellChange will see the new
	//   player cell and trigger a refresh against fully-loaded data.
	if (globals::state && globals::state->IsMainOrLoadingMenuOpen())
		return;

	// ---- 1) Cell-change detection ----
	if (DetectCellChange()) {
		// `cachedCellX/Y`, `worldOriginX/Y`, `fillRow`, `ready`, and `waterMaskCPU`
		// have all been reset by RebaseToNewCell.  Fill will progress from here.
	}

	// ---- 2 & 3) Progressive fill + upload ----
	if (!ready) {
		if (FillWaterMaskRows(kFillRowsPerFrame)) {
			UploadWaterMaskToGPU();
			ready = true;
		}
	}

	// ---- 4) Scene height broker readiness (Layer 1.2) ----
	//   Just-the-flag updates here.  Actual SRV binding is the consumer's
	//   responsibility — see comment below.
	DetectSceneHeightReadiness();

	// ---- 5) Try to finalize any pending CPU mirror readbacks (Layer 3 Stage 1) ----
	//   DO_NOT_WAIT map: succeeds if the GPU finished the staging copy issued
	//   in the prior frame's DispatchSnowMaskCSIfNeeded, otherwise retries
	//   next frame.  Bumps cpuMirrorsGeneration on success so SnowDeformation
	//   slab updates pick up the new data.
	TryFinalizeCPUMirrorReadbacks();

	// ---- SRV bindings are NOT performed here (anymore) ----
	//   We originally bound the water mask at PS t36 and Skylighting's borrowed
	//   texOcclusion at PS t37 every Prepass.  That broke on AMD with a GPU TDR.
	//
	//   Root cause: Skylighting's `RenderOcclusion` (Skylighting.cpp:534-589)
	//   binds `texOcclusion->dsv` as the depth target and renders into it
	//   every frame.  Our persistent SRV binding of the same resource at t37
	//   created a same-resource-SRV+DSV conflict.  D3D11's debug layer warns
	//   ("RESOURCE_USAGE_CONFLICT") and auto-unbinds; AMD's RX 7900 driver
	//   appears to corrupt state through that auto-unbind path mid-render,
	//   triggering a TDR with no CPU-side crash log.
	//
	//   Correct pattern: SceneHeight EXPOSES the data products via getters
	//   (GetWaterMaskSRV(), GetSceneOcclusionSRV(), GetWaterMask*Slot()) and
	//   CONSUMERS bind them just-in-time inside the dispatch/pass that
	//   samples them — then unbind, or let the engine's next draw call
	//   overwrite the slot naturally.  No speculative binding.
	//
	//   Until Layer 2 lands, nothing samples the data → nothing needs to bind.
	//   The cbuffer entry (PerFrame) is still populated, so any shader that
	//   wants to check readiness flags can do so without an SRV bind.
}

// =============================================================================
// DetectCellChange
//   Compares the player's current cell coordinates against the cached pair.
//   On mismatch, calls RebaseToNewCell and returns true.
// =============================================================================
bool SceneHeight::DetectCellChange()
{
	auto eyePos = Util::GetEyePosition(0);
	const int cellX = static_cast<int>(std::floor(eyePos.x / kCellSize));
	const int cellY = static_cast<int>(std::floor(eyePos.y / kCellSize));

	if (cellX == cachedCellX && cellY == cachedCellY)
		return false;

	RebaseToNewCell(cellX, cellY);
	return true;
}

// =============================================================================
// RebaseToNewCell
//   Centers the cached window on the new player cell.  The window's SW corner
//   sits (kCoverage / 2) units west and south of the cell center.
// =============================================================================
void SceneHeight::RebaseToNewCell(int newCellX, int newCellY)
{
	cachedCellX = newCellX;
	cachedCellY = newCellY;

	const float cellCenterX = (static_cast<float>(newCellX) + 0.5f) * kCellSize;
	const float cellCenterY = (static_cast<float>(newCellY) + 0.5f) * kCellSize;
	worldOriginX = cellCenterX - kCoverage * 0.5f;
	worldOriginY = cellCenterY - kCoverage * 0.5f;

	std::fill(waterMaskCPU.begin(), waterMaskCPU.end(), kNoWaterSentinel);
	fillRow = 0;
	ready   = false;

	// Snow mask depends on the water mask + scene height for the new cell —
	// invalidate so DispatchSnowMaskCSIfNeeded re-runs once the prerequisites
	// settle.  Downstream consumers see SnowMaskReady=0 in the cbuffer during
	// this window and gracefully default to "allow snow".
	snowMaskValid = false;

	logger::info("[SceneHeight] cell change → ({}, {}), origin ({:.0f}, {:.0f}), refreshing water + snow masks",
		newCellX, newCellY, worldOriginX, worldOriginY);
}

// =============================================================================
// FillWaterMaskRows
//   Populates up to `rowCount` rows of the water mask CPU buffer by sampling
//   RE::TES::GetWaterHeight at each texel's center world position.  Returns
//   true if this call advanced `fillRow` to kResolution (refresh complete).
// =============================================================================
bool SceneHeight::FillWaterMaskRows(uint32_t rowCount)
{
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return false;

	const float texelSize = kCoverage / static_cast<float>(kResolution);
	const uint32_t endRow = std::min(fillRow + rowCount, kResolution);

	for (uint32_t row = fillRow; row < endRow; ++row) {
		const float worldY = worldOriginY + (static_cast<float>(row) + 0.5f) * texelSize;
		for (uint32_t col = 0; col < kResolution; ++col) {
			const float worldX = worldOriginX + (static_cast<float>(col) + 0.5f) * texelSize;

			// Engine's water-height query — finds the TESObjectCELL containing
			// (x, y), then evaluates the cell's water plane (or worldspace
			// default water) at that XY.  Returns -NI_INFINITY when no water
			// body is associated with the cell.  Z=0 in the query position is
			// fine; the query is XY-driven (water surfaces are horizontal).
			const RE::NiPoint3 pos{ worldX, worldY, 0.0f };
			RE::TESObjectCELL* cell = tes->GetCell(pos);

			float height = kNoWaterSentinel;
			if (cell != nullptr) {
				height = tes->GetWaterHeight(pos, cell);
				if (height <= -RE::NI_INFINITY * 0.5f) {
					height = kNoWaterSentinel;
				}
			}

			waterMaskCPU[row * kResolution + col] = height;
		}
	}

	fillRow = endRow;
	return fillRow >= kResolution;
}

// =============================================================================
// UploadWaterMaskToGPU
//   Single subresource update — pushes the fully-populated CPU shadow into the
//   R32_FLOAT texture.  No format conversion needed.
// =============================================================================
void SceneHeight::UploadWaterMaskToGPU()
{
	auto context = globals::d3d::context;
	if (!context || !waterMaskTex)
		return;

	const UINT rowPitch = kResolution * static_cast<UINT>(sizeof(float));
	context->UpdateSubresource(
		waterMaskTex->resource.get(), 0, nullptr,
		waterMaskCPU.data(),
		rowPitch, 0);
}

// =============================================================================
// BindToPS / UnbindFromPS
//   Consumer-driven SRV binding.  Call BindToPS() inside a dispatch / draw
//   that samples scene-height data, then UnbindFromPS() (or just let the
//   next draw overwrite the slots).
//
//   These are NOT called from Prepass.  Persistent SRV binding of
//   Skylighting's texOcclusion at t37 conflicted with Skylighting's own
//   per-frame DSV bind of the same resource, crashing AMD drivers via TDR.
//   Just-in-time binding inside the consumer's own pass avoids the conflict
//   entirely — when the consumer is sampling, Skylighting is not writing.
// =============================================================================
void SceneHeight::BindToPS()
{
	auto context = globals::d3d::context;
	if (!context)
		return;

	// Bind snow mask (t35), water mask (t36), scene occlusion (t37) as a
	// contiguous triple — single API call.
	ID3D11ShaderResourceView* srvs[3] = {
		snowMaskTex ? snowMaskTex->srv.get() : nullptr,
		waterMaskTex ? waterMaskTex->srv.get() : nullptr,
		globals::features::skylighting.loaded
			? globals::features::skylighting.GetSceneOcclusionSRV()
			: nullptr
	};
	context->PSSetShaderResources(kSnowMaskPSSlot, 3, srvs);
}

void SceneHeight::UnbindFromPS()
{
	auto context = globals::d3d::context;
	if (!context)
		return;

	ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(kSnowMaskPSSlot, 3, nulls);
}

// =============================================================================
// DispatchSnowMaskCSIfNeeded
//   Layer 2 driver.  Called once per frame from Deferred::DeferredPasses.
//   Cheap fast path when nothing to do (single bool check, early return).
//
//   Dispatch preconditions:
//     - CS compiled and snow-mask texture allocated
//     - Water mask `ready` (Layer 1.1 finished progressive fill)
//     - Scene height `sceneHeightReady` (Skylighting loaded + texOcclusion bound)
//     - snowMaskValid == false (don't re-dispatch when cached result is current)
//
//   Bindings, applied just-in-time inside the dispatch:
//     t0 = water mask SRV
//     t1 = Skylighting's texOcclusion SRV (borrowed; safe here because
//          DeferredPasses runs after Skylighting::RenderOcclusion releases
//          the DSV — no SRV+DSV conflict)
//     s0 = our linear-clamp sampler
//     u0 = snow mask UAV
//     b5 = SharedData cbuffer (already bound by Deferred — we don't re-bind)
//
//   Cleanup: unbind UAV + SRVs before returning so the next dispatch in the
//   frame doesn't see stale bindings.
// =============================================================================
void SceneHeight::DispatchSnowMaskCSIfNeeded()
{
	if (!loaded)
		return;
	if (snowMaskValid)
		return;
	if (!ready || !sceneHeightReady)
		return;
	if (!snowMaskCS || !snowMaskTex || !waterMaskTex)
		return;
	if (!linearClampSampler)
		return;

	auto* skylightingSRV = globals::features::skylighting.GetSceneOcclusionSRV();
	if (!skylightingSRV)
		return;

	auto context = globals::d3d::context;
	if (!context)
		return;

	// Save current CS state so we don't trample on whatever Deferred had set up.
	// Deferred::DeferredPasses runs other CS dispatches around our call; clean
	// scope restoration prevents cross-contamination.
	ID3D11ComputeShader*       prevCS         = nullptr;
	ID3D11ShaderResourceView*  prevSRVs[2]    = { nullptr, nullptr };
	ID3D11UnorderedAccessView* prevUAVs[2]    = { nullptr, nullptr };
	ID3D11SamplerState*        prevSampler    = nullptr;
	context->CSGetShader(&prevCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 2, prevSRVs);
	context->CSGetUnorderedAccessViews(0, 2, prevUAVs);
	context->CSGetSamplers(0, 1, &prevSampler);

	// Bind our CS resources.
	ID3D11ShaderResourceView* srvs[2] = {
		waterMaskTex->srv.get(),
		skylightingSRV
	};
	context->CSSetShaderResources(0, 2, srvs);

	ID3D11SamplerState* samp = linearClampSampler.get();
	context->CSSetSamplers(0, 1, &samp);

	// Two UAV outputs now: u0 = snow mask, u1 = scene-height world Z.
	ID3D11UnorderedAccessView* uavs[2] = {
		snowMaskTex->uav.get(),
		sceneHeightWorldTex ? sceneHeightWorldTex->uav.get() : nullptr
	};
	UINT initialCounts[2] = { 0, 0 };  // ignored for non-append/consume UAVs
	context->CSSetUnorderedAccessViews(0, 2, uavs, initialCounts);

	context->CSSetShader(snowMaskCS.get(), nullptr, 0);

	// Dispatch — 128 / 8 = 16 thread groups per axis.
	const UINT groupDim = (kResolution + 7) / 8;
	context->Dispatch(groupDim, groupDim, 1);

	// Restore previous bindings.  Order matters: unbind UAVs BEFORE restoring
	// SRVs, otherwise D3D11 will complain that the resource backing the UAV
	// is also bound as SRV (we restore prevSRVs which might overlap).
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, initialCounts);

	context->CSSetShaderResources(0, 2, prevSRVs);
	context->CSSetSamplers(0, 1, &prevSampler);
	context->CSSetUnorderedAccessViews(0, 2, prevUAVs, initialCounts);
	context->CSSetShader(prevCS, nullptr, 0);

	// Release the COM refs CSGet*() bumped.
	if (prevCS)        prevCS->Release();
	if (prevSRVs[0])   prevSRVs[0]->Release();
	if (prevSRVs[1])   prevSRVs[1]->Release();
	if (prevUAVs[0])   prevUAVs[0]->Release();
	if (prevUAVs[1])   prevUAVs[1]->Release();
	if (prevSampler)   prevSampler->Release();

	snowMaskValid = true;
	logger::info("[SceneHeight] SnowMask CS dispatched — cell ({}, {})",
		cachedCellX, cachedCellY);

	// Layer 3 Stage 1: queue CPU mirror readbacks now that the GPU products
	// are current.  Map happens on a subsequent frame in Prepass to avoid
	// CPU/GPU sync stall (D3D11 Map on staging blocks until the copy finishes).
	IssueCPUMirrorReadbacks();
}

// =============================================================================
// IssueCPUMirrorReadbacks
//   Triggers GPU→staging copies of snowMaskTex + Skylighting's texOcclusion.
//   Maps happen on a later frame (Prepass calls TryFinalizeCPUMirrorReadbacks).
// =============================================================================
void SceneHeight::IssueCPUMirrorReadbacks()
{
	auto context = globals::d3d::context;
	if (!context)
		return;
	if (!snowMaskTex || !snowMaskStaging)
		return;
	if (!sceneHeightWorldTex || !sceneHeightStaging)
		return;

	// Both source and staging are R-format 128×128 textures created by us,
	// so CopyResource is format-compatible and zero-conversion.
	context->CopyResource(snowMaskStaging->resource.get(),     snowMaskTex->resource.get());
	context->CopyResource(sceneHeightStaging->resource.get(),  sceneHeightWorldTex->resource.get());

	readbackPending = true;
}

// =============================================================================
// TryFinalizeCPUMirrorReadbacks
//   Called each Prepass when a readback is pending.  Attempts D3D11_MAP_READ
//   with DO_NOT_WAIT — succeeds when the GPU has finished the copy.  On
//   success, copies staging contents to CPU vectors, bumps generation
//   counter, marks mirrors valid.
// =============================================================================
void SceneHeight::TryFinalizeCPUMirrorReadbacks()
{
	if (!readbackPending)
		return;

	auto context = globals::d3d::context;
	if (!context || !snowMaskStaging)
		return;

	// 1) Try snow mask map.  DO_NOT_WAIT returns immediately if the GPU is
	//    still working — we'll retry next frame.
	D3D11_MAPPED_SUBRESOURCE snowMapped{};
	const HRESULT snowHR = context->Map(
		snowMaskStaging->resource.get(), 0,
		D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
		&snowMapped);
	if (snowHR == DXGI_ERROR_WAS_STILL_DRAWING || FAILED(snowHR))
		return;  // try again next frame

	// Resize CPU mirror if needed (first map after re-init).
	if (snowMaskCPU.size() != static_cast<size_t>(kResolution) * kResolution)
		snowMaskCPU.assign(static_cast<size_t>(kResolution) * kResolution, 0);

	// Row-by-row copy — staging may have RowPitch padding.
	const uint8_t* src = static_cast<const uint8_t*>(snowMapped.pData);
	uint8_t*       dst = snowMaskCPU.data();
	for (uint32_t r = 0; r < kResolution; ++r) {
		std::memcpy(dst + r * kResolution, src + r * snowMapped.RowPitch, kResolution);
	}
	context->Unmap(snowMaskStaging->resource.get(), 0);

	// 2) Scene-height world-Z map — same dims (128×128 R32_FLOAT) as snow mask.
	if (sceneHeightStaging) {
		D3D11_MAPPED_SUBRESOURCE sceneMapped{};
		const HRESULT sceneHR = context->Map(
			sceneHeightStaging->resource.get(), 0,
			D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
			&sceneMapped);
		if (sceneHR == DXGI_ERROR_WAS_STILL_DRAWING || FAILED(sceneHR)) {
			// Snow mask succeeded but scene height isn't ready — leave pending.
			return;
		}

		if (sceneHeightCPU.size() != static_cast<size_t>(kResolution) * kResolution)
			sceneHeightCPU.assign(static_cast<size_t>(kResolution) * kResolution, 0.0f);

		const uint8_t* srcRow   = static_cast<const uint8_t*>(sceneMapped.pData);
		const size_t   rowBytes = static_cast<size_t>(kResolution) * sizeof(float);
		float*         dstRow   = sceneHeightCPU.data();
		for (uint32_t r = 0; r < kResolution; ++r) {
			std::memcpy(dstRow + r * kResolution, srcRow + r * sceneMapped.RowPitch, rowBytes);
		}
		context->Unmap(sceneHeightStaging->resource.get(), 0);
	}

	readbackPending      = false;
	cpuMirrorsValid      = true;
	++cpuMirrorsGeneration;

	logger::info("[SceneHeight] CPU mirrors refreshed — generation {}", cpuMirrorsGeneration);

	// -------------------------------------------------------------------------
	// DIAGNOSTIC: dump sceneHeightCPU stats + slices so we can see what the
	// CS actually produced.  Aiming to distinguish:
	//   - "mostly sentinel" (CS rejecting all texels) — min ≈ -1e10
	//   - "uniform value"   (texOcclusion is far-plane / our math is constant)
	//   - "tilted plane"    (M[2][0]/M[2][1] dominating, ndcZ ≈ constant)
	//   - "terrain-like"    (varied per-texel, physical magnitudes)
	// Logged once per successful readback (cell change → CS dispatch → readback).
	// -------------------------------------------------------------------------
	if (!sceneHeightCPU.empty()) {
		// Pass 1: stats (skip sentinel-valued texels for min/max so a single
		// rejected texel doesn't drown the real range).
		float  minVal     =  std::numeric_limits<float>::max();
		float  maxVal     = -std::numeric_limits<float>::max();
		double sumVal     = 0.0;
		size_t validCount = 0;
		size_t sentinelCount = 0;
		const float sentinelThreshold = kNoSceneHeightSentinel * 0.5f;
		for (float z : sceneHeightCPU) {
			if (z <= sentinelThreshold) {
				++sentinelCount;
				continue;
			}
			minVal  = std::min(minVal, z);
			maxVal  = std::max(maxVal, z);
			sumVal += z;
			++validCount;
		}
		const double mean = validCount ? sumVal / static_cast<double>(validCount) : 0.0;
		logger::info("[SceneHeight] heightmap stats: valid={} sentinel={} min={:.1f} max={:.1f} mean={:.1f} range={:.1f}",
			validCount, sentinelCount,
			validCount ? minVal : 0.0f,
			validCount ? maxVal : 0.0f,
			mean,
			validCount ? (maxVal - minVal) : 0.0f);

		// Pass 2: horizontal slice across mid-row.  Shows X-direction variance.
		const uint32_t midRow = kResolution / 2;
		std::string hStrip;
		hStrip.reserve(256);
		for (uint32_t i = 0; i < 16; ++i) {
			const uint32_t col = (i * kResolution) / 16;
			const float v = sceneHeightCPU[midRow * kResolution + col];
			if (v <= sentinelThreshold)
				hStrip += "  ----- ";
			else
				hStrip += std::format(" {:>6.0f} ", v);
		}
		logger::info("[SceneHeight] H-slice @ row {}: [{}]", midRow, hStrip);

		// Pass 3: vertical slice across mid-col.  Shows Y-direction variance.
		const uint32_t midCol = kResolution / 2;
		std::string vStrip;
		vStrip.reserve(256);
		for (uint32_t i = 0; i < 16; ++i) {
			const uint32_t row = (i * kResolution) / 16;
			const float v = sceneHeightCPU[row * kResolution + midCol];
			if (v <= sentinelThreshold)
				vStrip += "  ----- ";
			else
				vStrip += std::format(" {:>6.0f} ", v);
		}
		logger::info("[SceneHeight] V-slice @ col {}: [{}]", midCol, vStrip);

		// Pass 4: the camera's current absolute Z and worldOrigin so we can
		// interpret the values above (e.g., is the data clustered around the
		// player's Z, suggesting camera-coupling? Or unrelated to cam Z?).
		const auto cam = Util::GetEyePosition(0);
		logger::info("[SceneHeight] context: cam=({:.0f},{:.0f},{:.0f}) origin=({:.0f},{:.0f}) coverage={:.0f}",
			cam.x, cam.y, cam.z, worldOriginX, worldOriginY, kCoverage);
	}
}

// =============================================================================
// SampleSnowMaskCPU
//   Point-samples the CPU snow-mask mirror at a world position.  Returns 1.0
//   (allowed) when mirrors aren't ready or position is outside the cached
//   window — graceful fallback that lets snow appear everywhere until the
//   mask catches up.
// =============================================================================
float SceneHeight::SampleSnowMaskCPU(float worldX, float worldY) const
{
	if (!cpuMirrorsValid || snowMaskCPU.empty())
		return 1.0f;

	// World XY → mask UV in the cached 10K window.
	const float u = (worldX - worldOriginX) / kCoverage;
	const float v = (worldY - worldOriginY) / kCoverage;
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return 1.0f;

	const uint32_t cx = std::min(kResolution - 1u,
		static_cast<uint32_t>(u * static_cast<float>(kResolution)));
	const uint32_t cy = std::min(kResolution - 1u,
		static_cast<uint32_t>(v * static_cast<float>(kResolution)));

	return snowMaskCPU[cy * kResolution + cx] / 255.0f;
}

// =============================================================================
// SampleSceneHeightCPU
//   Direct lookup into sceneHeightCPU using the water-mask coord system
//   (worldX/Y → 10K window UV → 128 texel grid).  The CS already did the
//   inverse-projection from NDC z → world Z and wrote the world-Z values
//   directly into the texture we readback.  No matrix math here.
// =============================================================================
float SceneHeight::SampleSceneHeightCPU(float worldX, float worldY) const
{
	if (!cpuMirrorsValid || sceneHeightCPU.empty())
		return kNoSceneHeightSentinel;

	const float u = (worldX - worldOriginX) / kCoverage;
	const float v = (worldY - worldOriginY) / kCoverage;
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return kNoSceneHeightSentinel;

	const uint32_t cx = std::min(kResolution - 1u,
		static_cast<uint32_t>(u * static_cast<float>(kResolution)));
	const uint32_t cy = std::min(kResolution - 1u,
		static_cast<uint32_t>(v * static_cast<float>(kResolution)));

	const float worldZ = sceneHeightCPU[cy * kResolution + cx];

	// CS writes the sentinel for "no data" texels (out of texOcclusion window,
	// or Skylighting inactive).  Pass through as-is — caller checks against
	// kNoSceneHeightSentinel.
	return worldZ;
}

// =============================================================================
// DetectSceneHeightReadiness
//   Updates `sceneHeightReady` based on Skylighting's current state.  Cheap
//   (just pointer checks) so it's safe to call every frame from Prepass.
//   Edge transitions are logged once so misconfiguration is debuggable from
//   the CS log without per-frame spam.
// =============================================================================
void SceneHeight::DetectSceneHeightReadiness()
{
	const bool wasReady = sceneHeightReady;
	sceneHeightReady = globals::features::skylighting.loaded &&
	                   globals::features::skylighting.GetSceneOcclusionSRV() != nullptr;

	if (sceneHeightReady != wasReady) {
		if (sceneHeightReady) {
			logger::info("[SceneHeight] scene height broker active — Skylighting texOcclusion bound at t{}",
				kSceneOcclusionPSSlot);
		} else {
			logger::info("[SceneHeight] scene height broker inactive — Skylighting unloaded or texOcclusion missing");
		}
	}
}

// =============================================================================
// GetCommonBufferData
//   Pure read — never mutates feature state.  Called by FeatureBuffer once
//   per frame as part of the SharedData cbuffer assembly.
// =============================================================================
SceneHeight::PerFrame SceneHeight::GetCommonBufferData() const
{
	PerFrame data{};
	data.WaterMaskWorldOriginX = worldOriginX;
	data.WaterMaskWorldOriginY = worldOriginY;
	data.WaterMaskInvCoverage  = 1.0f / kCoverage;
	data.WaterMaskReady        = ready ? 1.0f : 0.0f;
	data.SceneHeightReady      = sceneHeightReady ? 1.0f : 0.0f;
	data.SnowMaskReady         = snowMaskValid ? 1.0f : 0.0f;
	return data;
}

// =============================================================================
// DrawSettings
//   Debug-only UI.  No user-tunable settings yet — this is infrastructure.
//   Shows live state + texture previews for verification of Layer 1.1 + 1.2.
// =============================================================================
void SceneHeight::DrawSettings()
{
	ImGui::TextDisabled("SceneHeight is foundation infrastructure — no user settings.");
	ImGui::Separator();

	// ---- Layer 1.1: Water mask state ----
	if (ImGui::CollapsingHeader("Water Mask (Layer 1.1)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Cached cell: (%d, %d)", cachedCellX, cachedCellY);
		ImGui::Text("World origin: (%.0f, %.0f)", worldOriginX, worldOriginY);
		ImGui::Text("Coverage: %.0f world units, %u×%u texels (%.1f units/texel)",
			kCoverage, kResolution, kResolution, kCoverage / static_cast<float>(kResolution));
		ImGui::Text("State: %s (fill row %u / %u)",
			ready ? "READY" : "FILLING", fillRow, kResolution);

		if (ImGui::Button("Force Water Mask Refresh")) {
			InvalidateCache();
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Invalidates the cached water mask, triggering progressive re-fill on the next Prepass.");
	}

	// ---- Layer 1.2: Scene height broker state ----
	if (ImGui::CollapsingHeader("Scene Height Broker (Layer 1.2)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Skylighting loaded: %s",
			globals::features::skylighting.loaded ? "YES" : "NO");
		auto* occlusionSRV = globals::features::skylighting.GetSceneOcclusionSRV();
		ImGui::Text("texOcclusion bound: %s @ t%u",
			occlusionSRV ? "YES" : "NO", kSceneOcclusionPSSlot);
		ImGui::Text("State: %s", sceneHeightReady ? "READY" : "INACTIVE");
		if (!sceneHeightReady)
			ImGui::TextDisabled("(Enable Skylighting to activate scene-height queries)");
	}

	// ---- Layer 2: Snow mask state ----
	if (ImGui::CollapsingHeader("Snow Mask (Layer 2)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("CS compiled: %s", snowMaskCS ? "YES" : "NO");
		ImGui::Text("Texture allocated: %s @ t%u",
			snowMaskTex ? "YES" : "NO", kSnowMaskPSSlot);
		ImGui::Text("Resolution: %u×%u (R8_UNORM, ~%.1f units/texel)",
			kResolution, kResolution, kCoverage / static_cast<float>(kResolution));
		ImGui::Text("Slope threshold: cos(70°) = %.3f", kSnowSlopeCosineThreshold);
		ImGui::Text("State: %s", snowMaskValid ? "VALID (cached for this cell)" : "PENDING");

		const char* preReq = "";
		if (!ready)             preReq = "(waiting on water mask fill)";
		else if (!sceneHeightReady) preReq = "(waiting on Skylighting)";
		else if (!snowMaskValid)    preReq = "(dispatching next Deferred pass)";
		if (preReq[0])
			ImGui::TextDisabled("%s", preReq);
	}

	// ---- Texture previews intentionally absent ----
	//   `ImGui::Image` rendering of our internal textures (water mask R32_FLOAT,
	//   Skylighting's depth-typed texOcclusion) crashed AMD drivers with no
	//   crash log — a GPU-side TDR, not a CPU fault we can catch.  Removed the
	//   previews entirely rather than chase the AMD-specific corruption path
	//   through ImGui's draw pipeline.
	//
	//   When future debugging needs visual feedback (verifying mask shape,
	//   confirming occluder detection, etc.), the right approach is an
	//   in-world overlay: render colorized markers at sampled XY points,
	//   highlight meshes that pass/fail the snow-allowed test, etc.  That
	//   stays inside the engine's normal render path and avoids the ImGui
	//   resource-bind interaction that's tripping the driver.
}

// =============================================================================
// Settings serialisation — empty for now (no user settings to persist).
// =============================================================================
void SceneHeight::LoadSettings(json&) {}
void SceneHeight::SaveSettings(json&) {}
void SceneHeight::RestoreDefaultSettings() {}
