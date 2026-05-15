#include "BloodDecalGrass.h"

#include "State.h"
#include "Utils/D3D.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	BloodDecalGrass::Settings,
	Enable,
	CorpseBloodRadius,
	SplatterBloodRadius,
	BloodIntensity,
	CorpseSoakDuration,
	CorpseDryIntensity,
	CorpseDryDuration,
	SplatterLifetime,
	SplatterFadeInTime,
	SplatterFadeOutTime,
	BloodColorR,
	BloodColorG,
	BloodColorB,
	EnableSurfaceStaining,
	SurfaceHeightThreshold,
	SurfaceNormalThreshold,
	EnableSurfaceFlow,
	DripReachMultiplier,
	DripRivuletWidth,
	CorpsePulseCount,
	CorpsePulseInterval,
	CorpsePulseDecay,
	EnableFluidSim,
	FluidViscosity,
	FluidDryingRate,
	FluidBloodVolumeRate,
	FluidEvaporationRate,
	FluidIterations,
	FluidMomentumStrength,
	FluidVelocityDamping,
	FlowNoiseScale,
	EnableBloodParallax,
	BloodParallaxDepth)

bool BloodDecalGrass::HasShaderDefine(RE::BSShader::Type shaderType)
{
	return shaderType == RE::BSShader::Type::Grass ||
	       shaderType == RE::BSShader::Type::Lighting;
}

RE::BSEventNotifyControl BloodDecalGrass::ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*)
{
	if (!a_event || !a_event->target || !settings.Enable)
		return RE::BSEventNotifyControl::kContinue;

	auto* target = a_event->target.get();
	if (!target || !target->Is3DLoaded())
		return RE::BSEventNotifyControl::kContinue;

	auto* actor = target->As<RE::Actor>();
	if (!actor)
		return RE::BSEventNotifyControl::kContinue;

	// Only outdoor hits matter for grass
	auto* cell = actor->GetParentCell();
	if (!cell || cell->IsInteriorCell())
		return RE::BSEventNotifyControl::kContinue;

	RE::NiPoint3 hitPos = actor->GetPosition();

	std::lock_guard lock(bloodMutex);

	uint totalEntries = (uint)trackedCorpses.size() + (uint)hitSplatters.size();
	if (totalEntries < MAX_BLOOD_ENTRIES) {
		HitSplatter splatter;
		splatter.position = hitPos;
		splatter.age = 0.0f;
		splatter.lifetime = settings.SplatterLifetime;
		hitSplatters.push_back(splatter);
	}

	return RE::BSEventNotifyControl::kContinue;
}

void BloodDecalGrass::ScanForDeadActors()
{
	RE::NiPoint3 cameraPos = Util::GetEyePosition(0);

	for (auto& corpse : trackedCorpses)
		corpse.stillPresent = false;

	auto processDeadActor = [&](RE::Actor* actor) {
		if (!actor || !actor->IsDead() || !actor->Is3DLoaded())
			return;

		auto* cell = actor->GetParentCell();
		if (!cell || cell->IsInteriorCell())
			return;

		float sqDist = cameraPos.GetSquaredDistance(actor->GetPosition());
		if (sqDist > MAX_SCAN_SQ_DISTANCE)
			return;

		RE::FormID formID = actor->GetFormID();

		for (auto& corpse : trackedCorpses) {
			if (corpse.actorFormID == formID) {
				corpse.stillPresent = true;

				if (corpse.presentTime < CORPSE_SETTLE_TIME) {
					// Ragdoll still settling — follow the body so blood ends up where it lands
					corpse.bloodOrigin = actor->GetPosition();
				} else if (!corpse.frozen) {
					// Body settled — freeze soak if corpse was dragged away from its blood origin
					float moveSqDist = corpse.bloodOrigin.GetSquaredDistance(actor->GetPosition());
					if (moveSqDist > CORPSE_MOVE_THRESHOLD_SQ)
						corpse.frozen = true;
				}
				return;
			}
		}

		uint totalEntries = (uint)trackedCorpses.size() + (uint)hitSplatters.size();
		if (totalEntries < MAX_BLOOD_ENTRIES) {
			TrackedCorpse corpse;
			corpse.actorFormID = formID;
			corpse.bloodOrigin = actor->GetPosition();
			corpse.presentTime = 0.0f;
			corpse.stillPresent = true;
			corpse.frozen = false;

			// Pulse injection: actor scale determines mass → pulse count
			float scale = actor->GetScale();
			corpse.mass = scale * scale * settings.CorpsePulseCount;
			corpse.totalPulses = std::max(1, (int)std::ceil(corpse.mass));
			corpse.pulsesRemaining = corpse.totalPulses;
			corpse.pulseTimer = 0.0f;
			corpse.pulseInterval = settings.CorpsePulseInterval;
			corpse.pulseIntensity = 1.0f;

			trackedCorpses.push_back(corpse);
			logger::debug("[BLOOD DECAL GRASS] Tracking new corpse {:08X} at ({:.0f}, {:.0f}, {:.0f}) mass:{:.1f} pulses:{}",
				formID, corpse.bloodOrigin.x, corpse.bloodOrigin.y, corpse.bloodOrigin.z,
				corpse.mass, corpse.totalPulses);
		}
	};

	if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
		for (auto& actorHandle : processLists->highActorHandles) {
			if (auto actor = actorHandle.get().get())
				processDeadActor(actor);
		}
		for (auto& actorHandle : processLists->middleHighActorHandles) {
			if (auto actor = actorHandle.get().get())
				processDeadActor(actor);
		}
	}
}

BloodDecalGrass::CommonBufferData BloodDecalGrass::GetCommonBufferData()
{
	CommonBufferData data;
	data.Enabled = settings.Enable ? 1 : 0;
	data.BloodIntensity = settings.BloodIntensity;
	data.EnableSurfaceStaining = (settings.Enable && settings.EnableSurfaceStaining) ? 1 : 0;
	data.SurfaceHeightThreshold = settings.SurfaceHeightThreshold;
	data.SurfaceNormalThreshold = settings.SurfaceNormalThreshold;
	data.EnableSurfaceFlow = (settings.Enable && settings.EnableSurfaceFlow) ? 1 : 0;
	data.DripReachMultiplier = settings.DripReachMultiplier;
	data.DripRivuletWidth = settings.DripRivuletWidth;

	// Fluid sim grid params
	data.EnableFluidSim = (settings.Enable && settings.EnableFluidSim) ? 1 : 0;

	if (data.EnableFluidSim) {
		auto eyePos = Util::GetEyePosition(0);

		int cellIDX = (int)round(eyePos.x / GRID_CELL_SIZE);
		int cellIDY = (int)round(eyePos.y / GRID_CELL_SIZE);

		if (prevCellIDX == INT_MAX) {
			// First frame — no scroll
			currentValidMarginX = 0;
			currentValidMarginY = 0;
		} else {
			currentValidMarginX = prevCellIDX - cellIDX;
			currentValidMarginY = prevCellIDY - cellIDY;
		}

		prevCellIDX = cellIDX;
		prevCellIDY = cellIDY;

		// Toroidal offset (ensure positive modulo)
		currentArrayOriginX = ((cellIDX - (int)GRID_DIM / 2) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
		currentArrayOriginY = ((cellIDY - (int)GRID_DIM / 2) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

		// World position of the grid's minimum corner
		currentGridWorldOriginX = (cellIDX - (int)GRID_DIM / 2) * GRID_CELL_SIZE;
		currentGridWorldOriginY = (cellIDY - (int)GRID_DIM / 2) * GRID_CELL_SIZE;

		data.GridCellSize = GRID_CELL_SIZE;
		data.GridWorldSize = GRID_WORLD_SIZE;
		data.GridWorldOriginX = currentGridWorldOriginX;
		data.GridWorldOriginY = currentGridWorldOriginY;
		data.ArrayOriginX = currentArrayOriginX;
		data.ArrayOriginY = currentArrayOriginY;
		data.ValidMarginX = currentValidMarginX;
		data.ValidMarginY = currentValidMarginY;
		data.RcpGridCellSize = 1.0f / GRID_CELL_SIZE;

		float dt = *globals::game::deltaTime * !globals::game::ui->GameIsPaused();
		data.DeltaTime = dt;
		data.Viscosity = settings.FluidViscosity;
		data.DryingRate = settings.FluidDryingRate;
		data.BloodVolumeRate = settings.FluidBloodVolumeRate;
		data.EvaporationRate = settings.FluidEvaporationRate;

		data.IterationCount = (uint)std::clamp(settings.FluidIterations, 1, 8);
		data.MomentumStrength = settings.FluidMomentumStrength;
		data.VelocityDamping = settings.FluidVelocityDamping;
	}

	data.FlowNoiseScale = settings.FlowNoiseScale;
	data.ParallaxDepthScale = settings.BloodParallaxDepth;
	data.EnableParallax = (settings.Enable && settings.EnableBloodParallax && settings.EnableFluidSim) ? 1 : 0;
	data.BloodColorR = settings.BloodColorR;
	data.BloodColorG = settings.BloodColorG;
	data.BloodColorB = settings.BloodColorB;

	std::lock_guard lock(bloodMutex);
	data.EntryCount = std::min((uint)(trackedCorpses.size() + hitSplatters.size()), MAX_BLOOD_ENTRIES);
	return data;
}

void BloodDecalGrass::Update()
{
	static Util::FrameChecker frameChecker;
	if (!frameChecker.IsNewFrame())
		return;

	if (!settings.Enable)
		return;

	auto context = globals::d3d::context;

	std::lock_guard lock(bloodMutex);

	float dt = *globals::game::deltaTime * !globals::game::ui->GameIsPaused();

	ScanForDeadActors();

	// Advance corpse timers and pulse injection, remove ones no longer present
	trackedCorpses.erase(
		eastl::remove_if(trackedCorpses.begin(), trackedCorpses.end(), [&](TrackedCorpse& c) {
			if (c.stillPresent && !c.frozen) {
				c.presentTime += dt;

				// Advance pulse injection timer
				if (c.pulsesRemaining > 0) {
					c.pulseTimer += dt;
					if (c.pulseTimer >= c.pulseInterval) {
						c.pulsesRemaining--;
						c.pulseTimer = 0.0f;
						c.pulseInterval *= settings.CorpsePulseDecay;
						c.pulseIntensity *= 0.85f;
					}
				}
			}
			return !c.stillPresent;
		}),
		trackedCorpses.end());

	// Advance splatter timers, remove expired ones
	hitSplatters.erase(
		eastl::remove_if(hitSplatters.begin(), hitSplatters.end(), [&](HitSplatter& s) {
			s.age += dt;
			return s.age >= s.lifetime;
		}),
		hitSplatters.end());

	uint corpseCount = (uint)trackedCorpses.size();
	uint splatterCount = (uint)hitSplatters.size();
	uint totalCount = std::min(corpseCount + splatterCount, MAX_BLOOD_ENTRIES);

	if (totalCount > 0 && bloodBuffer) {
		eastl::vector<BloodEntry> entries(totalCount);

		// Pack corpse entries first
		uint idx = 0;
		for (uint i = 0; i < corpseCount && idx < totalCount; i++, idx++) {
			auto& c = trackedCorpses[i];

			// Soak progress: monotonic 0→1 based on pulses delivered (drives noise shader reveal)
			float soakProgress = (c.totalPulses > 0)
				? 1.0f - (float)c.pulsesRemaining / (float)c.totalPulses
				: 1.0f;

			// Drying: after soak phase, intensity gradually settles to a lower value
			float dryProgress = std::clamp((c.presentTime - settings.CorpseSoakDuration) / settings.CorpseDryDuration, 0.0f, 1.0f);
			float intensity = std::lerp(1.0f, settings.CorpseDryIntensity, dryProgress);

			// Fluid sim injection rate: pulsed delivery (heartbeat-like)
			// During a pulse window (first 0.3s after pulse fires), injection is strong.
			// Between pulses, a small constant seep keeps blood trickling.
			// After all pulses, injection stops — blood just flows and coagulates.
			bool inPulseWindow = c.pulseTimer < 0.3f && c.pulsesRemaining > 0;
			float baseSeep = c.pulsesRemaining > 0 ? 0.15f : 0.0f;
			float injectionRate = inPulseWindow ? c.pulseIntensity : baseSeep;

			entries[idx].positionRadius = { c.bloodOrigin.x, c.bloodOrigin.y, c.bloodOrigin.z, settings.CorpseBloodRadius };
			entries[idx].colorIntensity = { settings.BloodColorR, settings.BloodColorG, settings.BloodColorB, intensity };
			entries[idx].soakParams = { soakProgress, injectionRate, 0.0f, 0.0f };
		}

		// Pack splatter entries after (fully soaked instantly, injection = intensity)
		for (uint i = 0; i < splatterCount && idx < totalCount; i++, idx++) {
			auto& s = hitSplatters[i];
			float fadeIn = std::clamp(s.age / settings.SplatterFadeInTime, 0.0f, 1.0f);
			float fadeOut = 1.0f - std::clamp((s.age - (s.lifetime - settings.SplatterFadeOutTime)) / settings.SplatterFadeOutTime, 0.0f, 1.0f);
			float intensity = fadeIn * fadeOut;

			entries[idx].positionRadius = { s.position.x, s.position.y, s.position.z, settings.SplatterBloodRadius };
			entries[idx].colorIntensity = { settings.BloodColorR, settings.BloodColorG, settings.BloodColorB, intensity };
			entries[idx].soakParams = { 1.0f, intensity, 0.0f, 0.0f };
		}

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(bloodBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(BloodEntry) * totalCount;
		memcpy_s(mapped.pData, bytes, entries.data(), bytes);
		context->Unmap(bloodBuffer->resource.get(), 0);
	}

	// Bind blood entry buffer for pixel shaders (noise-based path + fluid sim source)
	if (bloodBuffer) {
		ID3D11ShaderResourceView* srvs[] = { bloodBuffer->srv.get() };
		context->PSSetShaderResources(101, 1, srvs);
	}

	// Bind fluid sim output textures for pixel shaders
	if (settings.EnableFluidSim && texBloodHeight[0] && texBloodStain) {
		uint idx = simFrameIndex % 2;
		ID3D11ShaderResourceView* simSrvs[] = { texBloodHeight[idx]->srv.get(), texBloodStain->srv.get() };
		context->PSSetShaderResources(102, 2, simSrvs);

		// Terrain height for pixel shader height rejection (prevents blood on surfaces far above ground)
		if (texTerrainHeight) {
			ID3D11ShaderResourceView* terrainSrv = texTerrainHeight->srv.get();
			context->PSSetShaderResources(104, 1, &terrainSrv);
		}

		// Velocity texture for pixel shader flow noise orientation
		if (texBloodVelocity[idx]) {
			ID3D11ShaderResourceView* velSrv = texBloodVelocity[idx]->srv.get();
			context->PSSetShaderResources(105, 1, &velSrv);
		}

		ID3D11SamplerState* samp = gridSampler.get();
		context->PSSetSamplers(7, 1, &samp);
	}
}

void BloodDecalGrass::UpdateTerrainHeight()
{
	if (!texTerrainHeight)
		return;

	auto tes = RE::TES::GetSingleton();
	if (!tes)
		return;

	auto context = globals::d3d::context;

	// Lazy progressive fill: fill TERRAIN_FILL_ROWS_PER_FRAME rows per frame
	if (terrainFillRow < GRID_DIM) {
		for (uint r = 0; r < TERRAIN_FILL_ROWS_PER_FRAME && terrainFillRow < GRID_DIM; r++, terrainFillRow++) {
			for (uint c = 0; c < GRID_DIM; c++) {
				// Convert this texture coordinate to world position
				int localCellX = ((int)c - currentArrayOriginX % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
				int localCellY = ((int)terrainFillRow - currentArrayOriginY % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
				float worldX = currentGridWorldOriginX + (localCellX + 0.5f) * GRID_CELL_SIZE;
				float worldY = currentGridWorldOriginY + (localCellY + 0.5f) * GRID_CELL_SIZE;

				float height = 0.0f;
				RE::NiPoint3 queryPos(worldX, worldY, 0.0f);
				tes->GetLandHeight(queryPos, height);

				terrainHeightCPU[terrainFillRow * GRID_DIM + c] = height;
			}
		}

		// Upload entire terrain texture
		context->UpdateSubresource(
			texTerrainHeight->resource.get(), 0, nullptr,
			terrainHeightCPU.data(),
			GRID_DIM * sizeof(float), 0);
	}

	// Update scrolled strips when the grid moves
	if (currentValidMarginX != 0 || currentValidMarginY != 0) {
		bool updated = false;

		// For simplicity, mark terrain as needing a full refresh if scroll exceeds
		// a reasonable threshold. Normally scroll is 0-2 cells per frame.
		int absMarginX = abs(currentValidMarginX);
		int absMarginY = abs(currentValidMarginY);

		if (absMarginX > (int)GRID_DIM / 2 || absMarginY > (int)GRID_DIM / 2) {
			// Large scroll (teleport) — refill everything
			terrainFillRow = 0;
			return;
		}

		// Update the newly exposed rows and columns with terrain height
		for (uint row = 0; row < GRID_DIM; row++) {
			for (uint col = 0; col < GRID_DIM; col++) {
				int localCellX = ((int)col - currentArrayOriginX % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
				int localCellY = ((int)row - currentArrayOriginY % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

				bool isNew = false;
				if (currentValidMarginX > 0 && localCellX >= (int)GRID_DIM - currentValidMarginX)
					isNew = true;
				if (currentValidMarginX < 0 && localCellX < -currentValidMarginX)
					isNew = true;
				if (currentValidMarginY > 0 && localCellY >= (int)GRID_DIM - currentValidMarginY)
					isNew = true;
				if (currentValidMarginY < 0 && localCellY < -currentValidMarginY)
					isNew = true;

				if (isNew) {
					float worldX = currentGridWorldOriginX + (localCellX + 0.5f) * GRID_CELL_SIZE;
					float worldY = currentGridWorldOriginY + (localCellY + 0.5f) * GRID_CELL_SIZE;

					float height = 0.0f;
					RE::NiPoint3 queryPos(worldX, worldY, 0.0f);
					tes->GetLandHeight(queryPos, height);

					terrainHeightCPU[row * GRID_DIM + col] = height;
					updated = true;
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
}

void BloodDecalGrass::DispatchFluidSim(float dt)
{
	if (!bloodFlowCS || !texBloodHeight[0] || !texBloodHeight[1] || !texTerrainHeight || !texBloodStain)
		return;
	if (!texBloodVelocity[0] || !texBloodVelocity[1])
		return;
	if (!bloodBuffer || !iterationCB)
		return;

	auto context = globals::d3d::context;

	int iterations = std::clamp(settings.FluidIterations, 1, 8);
	float subDt = dt / (float)iterations;

	// Bind shared constant buffers for compute access (b5 = SharedData, b6 = FeatureData)
	ID3D11Buffer* sharedBufs[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBufs);

	// Bind iteration cbuffer at b7
	ID3D11Buffer* iterBuf = iterationCB.get();
	context->CSSetConstantBuffers(7, 1, &iterBuf);

	// Bind static SRVs: blood entries (t0), terrain height (t1)
	// t2 (blood height prev) and t3 (velocity prev) are bound per-iteration
	ID3D11ShaderResourceView* staticSrvs[] = {
		bloodBuffer->srv.get(),
		texTerrainHeight->srv.get()
	};
	context->CSSetShaderResources(0, 2, staticSrvs);

	context->CSSetShader(bloodFlowCS.get(), nullptr, 0);

	uint dispatchX = (GRID_DIM + 15) / 16;
	uint dispatchY = (GRID_DIM + 15) / 16;

	for (int iter = 0; iter < iterations; iter++) {
		uint prevIdx = (simFrameIndex + iter) % 2;
		uint currIdx = (simFrameIndex + iter + 1) % 2;

		// Update per-iteration cbuffer
		{
			FlowIterationCB iterData;
			iterData.IterationIndex = (uint)iter;
			iterData.SubDeltaTime = subDt;
			context->UpdateSubresource(iterationCB.get(), 0, nullptr, &iterData, 0, 0);
		}

		// Bind per-iteration SRVs: blood height prev (t2), velocity prev (t3)
		ID3D11ShaderResourceView* iterSrvs[] = {
			texBloodHeight[prevIdx]->srv.get(),
			texBloodVelocity[prevIdx]->srv.get()
		};
		context->CSSetShaderResources(2, 2, iterSrvs);

		// Bind UAVs: blood height curr (u0), stain (u1), velocity curr (u2)
		ID3D11UnorderedAccessView* uavs[] = {
			texBloodHeight[currIdx]->uav.get(),
			texBloodStain->uav.get(),
			texBloodVelocity[currIdx]->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

		context->Dispatch(dispatchX, dispatchY, 1);

		// Unbind UAVs between iterations to allow ping-pong swap
		ID3D11UnorderedAccessView* nullUavs[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);

		ID3D11ShaderResourceView* nullIterSrvs[2] = { nullptr, nullptr };
		context->CSSetShaderResources(2, 2, nullIterSrvs);
	}

	// Unbind all compute resources
	ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, 2, nullSrvs);
	context->CSSetShader(nullptr, nullptr, 0);

	simFrameIndex += iterations;
}

void BloodDecalGrass::Prepass()
{
	if (!settings.Enable || !settings.EnableFluidSim)
		return;

	float dt = *globals::game::deltaTime * !globals::game::ui->GameIsPaused();
	if (dt <= 0)
		return;

	UpdateTerrainHeight();
	DispatchFluidSim(dt);

	// Bind sim output textures for pixel shaders
	auto context = globals::d3d::context;
	uint idx = simFrameIndex % 2;

	if (texBloodHeight[idx] && texBloodStain) {
		ID3D11ShaderResourceView* simSrvs[] = { texBloodHeight[idx]->srv.get(), texBloodStain->srv.get() };
		context->PSSetShaderResources(102, 2, simSrvs);

		// Terrain height for pixel shader height rejection
		if (texTerrainHeight) {
			ID3D11ShaderResourceView* terrainSrv = texTerrainHeight->srv.get();
			context->PSSetShaderResources(104, 1, &terrainSrv);
		}

		// Velocity texture for pixel shader flow noise orientation
		if (texBloodVelocity[idx]) {
			ID3D11ShaderResourceView* velSrv = texBloodVelocity[idx]->srv.get();
			context->PSSetShaderResources(105, 1, &velSrv);
		}

		if (gridSampler) {
			ID3D11SamplerState* samp = gridSampler.get();
			context->PSSetSamplers(7, 1, &samp);
		}
	}
}

void BloodDecalGrass::SetupResources()
{
	auto device = globals::d3d::device;

	// Blood entry structured buffer (existing)
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(BloodEntry);
		sbDesc.ByteWidth = sizeof(BloodEntry) * MAX_BLOOD_ENTRIES;
		bloodBuffer = eastl::make_unique<Buffer>(sbDesc, nullptr, "BloodDecalGrass::BloodBuffer");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_BLOOD_ENTRIES;
		bloodBuffer->CreateSRV(srvDesc);
	}

	// Blood height ping-pong textures (R16G16_FLOAT: R=height, G=age)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = GRID_DIM;
		texDesc.Height = GRID_DIM;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		texDesc.SampleDesc = { 1, 0 };
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		texBloodHeight[0] = eastl::make_unique<Texture2D>(texDesc, "BloodDecalGrass::BloodHeight0");
		texBloodHeight[0]->CreateSRV(srvDesc);
		texBloodHeight[0]->CreateUAV(uavDesc);

		texBloodHeight[1] = eastl::make_unique<Texture2D>(texDesc, "BloodDecalGrass::BloodHeight1");
		texBloodHeight[1]->CreateSRV(srvDesc);
		texBloodHeight[1]->CreateUAV(uavDesc);
	}

	// Blood velocity ping-pong textures (R16G16_FLOAT: R=velX, G=velY)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = GRID_DIM;
		texDesc.Height = GRID_DIM;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		texDesc.SampleDesc = { 1, 0 };
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		texBloodVelocity[0] = eastl::make_unique<Texture2D>(texDesc, "BloodDecalGrass::BloodVelocity0");
		texBloodVelocity[0]->CreateSRV(srvDesc);
		texBloodVelocity[0]->CreateUAV(uavDesc);

		texBloodVelocity[1] = eastl::make_unique<Texture2D>(texDesc, "BloodDecalGrass::BloodVelocity1");
		texBloodVelocity[1]->CreateSRV(srvDesc);
		texBloodVelocity[1]->CreateUAV(uavDesc);
	}

	// Terrain height texture (R32_FLOAT, CPU-uploadable)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = GRID_DIM;
		texDesc.Height = GRID_DIM;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc = { 1, 0 };
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		texTerrainHeight = eastl::make_unique<Texture2D>(texDesc, "BloodDecalGrass::TerrainHeight");
		texTerrainHeight->CreateSRV(srvDesc);

		terrainHeightCPU.resize(GRID_DIM * GRID_DIM, 0.0f);
		terrainFillRow = 0;
	}

	// Blood stain map (R16_FLOAT, persistent stain intensity)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = GRID_DIM;
		texDesc.Height = GRID_DIM;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc = { 1, 0 };
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		texBloodStain = eastl::make_unique<Texture2D>(texDesc, "BloodDecalGrass::BloodStain");
		texBloodStain->CreateSRV(srvDesc);
		texBloodStain->CreateUAV(uavDesc);
	}

	// Wrap sampler for pixel shader grid sampling
	{
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, gridSampler.put()));
		Util::SetResourceName(gridSampler.get(), "BloodDecalGrass::GridSampler");
	}

	// Per-iteration constant buffer for multi-step fluid dispatch (b7, CS only)
	{
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(FlowIterationCB);
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		DX::ThrowIfFailed(device->CreateBuffer(&cbDesc, nullptr, iterationCB.put()));
		Util::SetResourceName(iterationCB.get(), "BloodDecalGrass::IterationCB");
	}

	CompileComputeShaders();
}

void BloodDecalGrass::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &bloodFlowCS, "BloodFlowCS.hlsl", {} },
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\BloodDecalGrass") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}

void BloodDecalGrass::ClearShaderCache()
{
	bloodFlowCS = nullptr;
	CompileComputeShaders();
}

void BloodDecalGrass::DrawSettings()
{
	if (ImGui::TreeNodeEx("Blood Decal Grass", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Blood Decal Grass", (bool*)&settings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Tints grass near dead actors and combat hits with blood.");
		}

		ImGui::SliderFloat("Blood Intensity", &settings.BloodIntensity, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Overall strength of the blood tinting effect.");
		}

		if (ImGui::TreeNodeEx("Corpse Blood", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Corpse Radius", &settings.CorpseBloodRadius, 32.0f, 512.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blood radius around dead actors (world units).");
			}

			ImGui::SliderFloat("Pulse Count", &settings.CorpsePulseCount, 1.0f, 20.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Base number of blood injection pulses for a normal-sized actor.\nLarger actors (higher scale) get proportionally more pulses.\nEach pulse is a heartbeat-like burst of blood.");
			}

			ImGui::SliderFloat("Pulse Interval", &settings.CorpsePulseInterval, 0.2f, 3.0f, "%.1f sec");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Starting interval between pulses (like a heartbeat).\nGets longer with each pulse as the body drains.");
			}

			ImGui::SliderFloat("Pulse Decay", &settings.CorpsePulseDecay, 1.0f, 2.0f, "%.2fx");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Multiplier on pulse interval after each beat.\n1.0 = constant rate, 2.0 = each beat takes twice as long.");
			}

			ImGui::SliderFloat("Soak Duration", &settings.CorpseSoakDuration, 1.0f, 30.0f, "%.1f sec");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Time before blood begins drying. Controls the noise-based soak reveal.");
			}

			ImGui::SliderFloat("Dry Intensity", &settings.CorpseDryIntensity, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Settled blood intensity after drying. Lower = more faded over time.");
			}

			ImGui::SliderFloat("Dry Duration", &settings.CorpseDryDuration, 5.0f, 120.0f, "%.0f sec");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How long after soaking for the blood to reach its dried intensity.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Hit Splatters", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Splatter Radius", &settings.SplatterBloodRadius, 16.0f, 256.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blood radius from combat hits (world units).");
			}

			ImGui::SliderFloat("Splatter Lifetime", &settings.SplatterLifetime, 10.0f, 300.0f, "%.0f sec");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How long hit splatters persist.");
			}

			ImGui::SliderFloat("Splatter Fade In", &settings.SplatterFadeInTime, 0.1f, 5.0f, "%.1f sec");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How quickly splatters appear.");
			}

			ImGui::SliderFloat("Splatter Fade Out", &settings.SplatterFadeOutTime, 5.0f, 60.0f, "%.0f sec");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How long splatters take to fade at end of life.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Surface Staining", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable Surface Staining", (bool*)&settings.EnableSurfaceStaining);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Tints non-grass surfaces (rocks, terrain, objects) near blood sources.\nFilters by surface normal and height to avoid painting walls and actors.");
			}

			ImGui::SliderFloat("Height Threshold", &settings.SurfaceHeightThreshold, 16.0f, 256.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Max vertical distance from blood source to stain a surface (world units).");
			}

			ImGui::SliderFloat("Normal Threshold", &settings.SurfaceNormalThreshold, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Minimum upward-facing angle to receive staining.\n0 = any angle, 0.5 = gentle slopes, 1.0 = only flat ground.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Surface Flow", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable Surface Flow", (bool*)&settings.EnableSurfaceFlow);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blood drips down sloped and vertical surfaces near blood sources.\nCreates rivulet streaks that extend over time.");
			}

			ImGui::SliderFloat("Drip Reach", &settings.DripReachMultiplier, 1.0f, 8.0f, "%.1fx radius");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How far drips can travel, as a multiplier of the blood source radius.");
			}

			ImGui::SliderFloat("Rivulet Width", &settings.DripRivuletWidth, 0.1f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Width of drip channels relative to source radius.\nSmaller = thinner rivulets, larger = broader flow.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Fluid Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable Fluid Simulation", (bool*)&settings.EnableFluidSim);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("GPU-accelerated shallow water simulation for blood.\nBlood pools in terrain depressions and flows downhill.\nWhen disabled, falls back to the noise-based soak effect.");
			}

			ImGui::SliderFloat("Viscosity", &settings.FluidViscosity, 0.0f, 0.99f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How thick/sluggish the blood is.\n0 = water-like, 0.99 = nearly frozen.");
			}

			ImGui::SliderFloat("Coagulation Rate", &settings.FluidDryingRate, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How quickly blood thickens over time.\nHigher = faster coagulation, blood stops flowing sooner.");
			}

			ImGui::SliderFloat("Volume Rate", &settings.FluidBloodVolumeRate, 0.001f, 0.5f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Blood volume injected per second per source.\nHigher = more blood, bigger pools.");
			}

			ImGui::SliderFloat("Evaporation", &settings.FluidEvaporationRate, 0.0f, 0.01f, "%.4f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How quickly blood evaporates/absorbs into the surface.\nVery slow by default.");
			}

			ImGui::Separator();

			ImGui::SliderInt("Iterations", &settings.FluidIterations, 1, 8);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Simulation steps per frame. Higher = more stable flow\nbut costs more GPU time. 2-4 recommended.");
			}

			ImGui::SliderFloat("Momentum", &settings.FluidMomentumStrength, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How much blood preserves its flow direction.\n0 = pure pressure relaxation, 1 = strong momentum.");
			}

			ImGui::SliderFloat("Velocity Damping", &settings.FluidVelocityDamping, 0.0f, 10.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How quickly velocity decays. Higher = blood slows faster.\nWorks with coagulation to stop old blood.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Flow Noise & Parallax", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Flow Noise Scale", &settings.FlowNoiseScale, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Intensity of flow-oriented noise that creates rivulet patterns\nat sub-grid resolution. 0 = smooth, 1 = very noisy.");
			}

			ImGui::Checkbox("Enable Blood Parallax", (bool*)&settings.EnableBloodParallax);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Parallax occlusion mapping on blood pools.\nCreates apparent depth without geometry.");
			}

			ImGui::SliderFloat("Parallax Depth", &settings.BloodParallaxDepth, 0.5f, 10.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Visual depth of blood pools in world units.\nHigher = deeper-looking puddles.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Blood Color", ImGuiTreeNodeFlags_DefaultOpen)) {
			float color[3] = { settings.BloodColorR, settings.BloodColorG, settings.BloodColorB };
			if (ImGui::ColorEdit3("Default Blood Color", color)) {
				settings.BloodColorR = color[0];
				settings.BloodColorG = color[1];
				settings.BloodColorB = color[2];
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("The color used to tint grass.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Debug Info")) {
			std::lock_guard lock(bloodMutex);
			ImGui::Text("Tracked Corpses: %u", (uint)trackedCorpses.size());
			ImGui::Text("Hit Splatters: %u", (uint)hitSplatters.size());
			ImGui::Text("Total Entries: %u / %u", (uint)(trackedCorpses.size() + hitSplatters.size()), MAX_BLOOD_ENTRIES);

			if (settings.EnableFluidSim) {
				ImGui::Separator();
				ImGui::Text("Grid: %ux%u (%.0f unit coverage)", GRID_DIM, GRID_DIM, GRID_WORLD_SIZE);
				ImGui::Text("Cell Size: %.1f units", GRID_CELL_SIZE);
				ImGui::Text("Array Origin: (%d, %d)", currentArrayOriginX, currentArrayOriginY);
				ImGui::Text("Grid World Origin: (%.0f, %.0f)", currentGridWorldOriginX, currentGridWorldOriginY);
				ImGui::Text("Terrain Fill: %u / %u rows", terrainFillRow, GRID_DIM);
				ImGui::Text("Sim Frame: %u (ping-pong: %u)", simFrameIndex, simFrameIndex % 2);
				ImGui::Text("Iterations/frame: %d", std::clamp(settings.FluidIterations, 1, 8));
			}

			for (uint i = 0; i < (uint)trackedCorpses.size(); i++) {
				auto& c = trackedCorpses[i];
				float soakProgress = (c.totalPulses > 0)
					? 1.0f - (float)c.pulsesRemaining / (float)c.totalPulses
					: 1.0f;
				ImGui::Text("  [%u] ID:%08X pos(%.0f,%.0f,%.0f) t:%.1f soak:%.2f pulse:%d/%d int:%.2f %s",
					i, c.actorFormID, c.bloodOrigin.x, c.bloodOrigin.y, c.bloodOrigin.z,
					c.presentTime, soakProgress, c.totalPulses - c.pulsesRemaining, c.totalPulses,
					c.pulseIntensity, c.frozen ? "FROZEN" : "");
			}
			ImGui::TreePop();
		}

		ImGui::TreePop();
	}
}

void BloodDecalGrass::LoadSettings(json& o_json)
{
	settings = o_json;
}

void BloodDecalGrass::SaveSettings(json& o_json)
{
	o_json = settings;
}

void BloodDecalGrass::RestoreDefaultSettings()
{
	settings = {};
}

void BloodDecalGrass::PostPostLoad()
{
	Hooks::Install();
}

void BloodDecalGrass::DataLoaded()
{
	if (auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton()) {
		sourceHolder->AddEventSink<RE::TESHitEvent>(this);
		logger::info("[BLOOD DECAL GRASS] Registered TESHitEvent sink");
	}
}

void BloodDecalGrass::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::bloodDecalGrass.Update();
	func(This, Pass, RenderFlags);
}

void BloodDecalGrass::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::bloodDecalGrass.Update();
	func(This, Pass, RenderFlags);
}

void BloodDecalGrass::Hooks::Install()
{
	stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[BLOOD DECAL GRASS] Installed hooks");
}
