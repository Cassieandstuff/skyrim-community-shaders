// Blood fluid simulation compute shader
// Implements momentum-biased pipe model with velocity tracking for blood
// pooling and flow. Multi-step iteration support via per-iteration cbuffer.
// Reads from previous frame's blood height + velocity (ping-pong), injects
// blood from source entries, redistributes via gravity-driven relaxation
// with momentum, and accumulates a persistent stain map.

#include "Common/SharedData.hlsli"

struct BloodDecalEntry
{
	float4 positionRadius;   // xyz = world position, w = radius
	float4 colorIntensity;   // xyz = blood color, w = overall intensity
	float4 soakParams;       // x = soak progress, y = injection rate
};

StructuredBuffer<BloodDecalEntry> BloodSources : register(t0);
Texture2D<float> TerrainHeight : register(t1);
Texture2D<float2> BloodHeightPrev : register(t2);
Texture2D<float2> BloodVelocityPrev : register(t3);

RWTexture2D<float2> BloodHeightCurr : register(u0);
RWTexture2D<float> BloodStain : register(u1);
RWTexture2D<float2> BloodVelocityCurr : register(u2);

// Per-iteration constant buffer (updated between dispatch calls)
cbuffer FlowIterationCB : register(b7)
{
	uint IterationIndex;
	float SubDeltaTime;
	uint iterPad0;
	uint iterPad1;
};

static const uint GRID_DIM = 512;

// 8-neighbor (Moore) stencil: cardinal + diagonal for isotropic diffusion.
// Without diagonals, pools form visible squares with X-pattern seams.
// distWeight corrects for the √2 longer distance to diagonal neighbors.
static const uint NUM_NEIGHBORS = 8;
static const int2 offsets[8] = {
	int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),   // cardinal
	int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)   // diagonal
};
static const float2 dirVectors[8] = {
	float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
	normalize(float2(1, 1)), normalize(float2(-1, 1)), normalize(float2(1, -1)), normalize(float2(-1, -1))
};
static const float distWeight[8] = {
	1.0, 1.0, 1.0, 1.0,                                             // cardinal: distance = 1 cell
	0.7071068, 0.7071068, 0.7071068, 0.7071068                       // diagonal: distance = √2 cells → weight = 1/√2
};

float2 TexCoordToWorld(uint2 tc)
{
	int2 arrayOrigin = SharedData::bloodDecalGrassSettings.ArrayOrigin;
	float2 gridWorldOrigin = SharedData::bloodDecalGrassSettings.GridWorldOrigin;
	float cellSize = SharedData::bloodDecalGrassSettings.GridCellSize;

	int2 localCell = (((int2)tc - arrayOrigin) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;
	return gridWorldOrigin + (float2(localCell) + 0.5) * cellSize;
}

bool IsNewCell(uint2 tc)
{
	int2 arrayOrigin = SharedData::bloodDecalGrassSettings.ArrayOrigin;
	int2 validMargin = SharedData::bloodDecalGrassSettings.ValidMargin;

	int2 localCell = (((int2)tc - arrayOrigin) % (int)GRID_DIM + (int)GRID_DIM) % (int)GRID_DIM;

	if (validMargin.x > 0 && localCell.x >= ((int)GRID_DIM - validMargin.x))
		return true;
	if (validMargin.x < 0 && localCell.x < (-validMargin.x))
		return true;
	if (validMargin.y > 0 && localCell.y >= ((int)GRID_DIM - validMargin.y))
		return true;
	if (validMargin.y < 0 && localCell.y < (-validMargin.y))
		return true;

	return false;
}

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	if (dtid.x >= GRID_DIM || dtid.y >= GRID_DIM)
		return;

	uint2 tc = dtid.xy;

	// Clear newly scrolled-in cells (only on first iteration)
	if (IterationIndex == 0 && IsNewCell(tc))
	{
		BloodHeightCurr[tc] = float2(0, 0);
		BloodStain[tc] = 0;
		BloodVelocityCurr[tc] = float2(0, 0);
		return;
	}

	float dt = SubDeltaTime;
	float viscosity = SharedData::bloodDecalGrassSettings.Viscosity;
	float dryingRate = SharedData::bloodDecalGrassSettings.DryingRate;
	float volumeRate = SharedData::bloodDecalGrassSettings.BloodVolumeRate;
	float evapRate = SharedData::bloodDecalGrassSettings.EvaporationRate;
	float momentumStrength = SharedData::bloodDecalGrassSettings.MomentumStrength;
	float velocityDamping = SharedData::bloodDecalGrassSettings.VelocityDamping;
	uint entryCount = SharedData::bloodDecalGrassSettings.EntryCount;
	float rcpCellSize = SharedData::bloodDecalGrassSettings.RcpGridCellSize;
	float cellSize = SharedData::bloodDecalGrassSettings.GridCellSize;

	if (dt <= 0)
	{
		BloodHeightCurr[tc] = BloodHeightPrev.Load(int3(tc, 0));
		BloodVelocityCurr[tc] = BloodVelocityPrev.Load(int3(tc, 0));
		return;
	}

	float2 worldPos = TexCoordToWorld(tc);
	float2 prev = BloodHeightPrev.Load(int3(tc, 0));
	float bloodH = prev.x;
	float bloodAge = prev.y;
	float terrainH = TerrainHeight.Load(int3(tc, 0));
	float2 velocity = BloodVelocityPrev.Load(int3(tc, 0));

	// === Phase 1: Inject blood from active sources (only on first iteration) ===
	if (IterationIndex == 0)
	{
		[loop]
		for (uint i = 0; i < entryCount; i++)
		{
			float2 srcXY = BloodSources[i].positionRadius.xy;
			float srcRadius = BloodSources[i].positionRadius.w;
			float srcIntensity = BloodSources[i].colorIntensity.w;
			float srcSoak = BloodSources[i].soakParams.y;

			float dist = distance(worldPos, srcXY);
			if (dist < srcRadius && srcIntensity > 0.001 && srcSoak > 0.001)
			{
				float t = dist / srcRadius;
				float falloff = saturate(1.0 - t);
				falloff *= falloff;

				// Use the full frame dt for injection (not subdivided)
				float fullDt = SharedData::bloodDecalGrassSettings.DeltaTime;
				float volume = falloff * srcIntensity * srcSoak * volumeRate * fullDt;
				bloodH += volume;

				if (volume > 0.001)
					bloodAge = lerp(bloodAge, 0.0, saturate(volume * 10.0));
			}
		}
	}

	// === Phase 2: Momentum-biased gravity-driven flow ===
	float surface = terrainH + bloodH;
	float ageFactor = 1.0 / (1.0 + bloodAge * dryingRate);
	float effectiveFlowRate = (1.0 - viscosity) * ageFactor;

	// Per-pipe outflow and direction accumulation for velocity update
	float netFlow = 0;
	float2 flowDirection = 0;  // weighted direction of net flow

	[unroll]
	for (uint n = 0; n < NUM_NEIGHBORS; n++)
	{
		uint2 neighborTC = ((int2)tc + offsets[n] + (int)GRID_DIM) % (int)GRID_DIM;
		float2 neighborData = BloodHeightPrev.Load(int3(neighborTC, 0));
		float neighborTerrainH = TerrainHeight.Load(int3(neighborTC, 0));
		float neighborSurface = neighborTerrainH + neighborData.x;

		float dw = distWeight[n];  // 1.0 for cardinal, 1/√2 for diagonal

		// Pressure difference: equalizes blood surface heights
		// Scale by distWeight — diagonal neighbors are farther, so gradient is weaker
		float pressureDiff = (surface - neighborSurface) * dw;

		// Terrain slope bias: drives blood downhill even when surfaces are level
		// rcpCellSize already handles cardinal distance; diagonal needs extra √2 correction
		float terrainSlope = (terrainH - neighborTerrainH) * rcpCellSize * dw;
		float slopeBias = terrainSlope * min(bloodH, 1.0) * 0.5;

		// Momentum bias: velocity in this direction pushes more blood that way.
		float momentumBias = dot(velocity, dirVectors[n]) * momentumStrength * min(bloodH, 1.0);

		// Combined driving force: pressure + gravity + momentum
		float drive = pressureDiff + slopeBias + momentumBias;

		if (drive > 0 && bloodH > 0.001)
		{
			// Outflow: blood leaves this cell
			float outflow = drive * effectiveFlowRate * dt;
			// CFL safety: 12.5% per neighbor (8 neighbors × 12.5% = 100% max total)
			outflow = min(outflow, bloodH * 0.125);
			netFlow -= outflow;
			flowDirection += dirVectors[n] * outflow;
		}
		else if (drive < 0 && neighborData.x > 0.001)
		{
			// Inflow: blood arrives from higher neighbor
			float neighborAge = neighborData.y;
			float neighborAgeFactor = 1.0 / (1.0 + neighborAge * dryingRate);
			float neighborFlowRate = (1.0 - viscosity) * neighborAgeFactor;

			// Neighbor's momentum toward us
			float2 neighborVel = BloodVelocityPrev.Load(int3(neighborTC, 0));
			float neighborMomentum = dot(neighborVel, -dirVectors[n]) * momentumStrength * min(neighborData.x, 1.0);

			float inDrive = (-drive) + max(neighborMomentum, 0);
			float inflow = inDrive * neighborFlowRate * dt;
			inflow = min(inflow, neighborData.x * 0.125);
			netFlow += inflow;
			// Inflow carries the neighbor's velocity direction into this cell
			flowDirection += neighborVel * inflow;
		}
	}

	bloodH = max(0, bloodH + netFlow);

	// === Phase 3: Velocity update ===
	// New velocity blends between:
	//   - Previous velocity (momentum persistence)
	//   - Flow-derived velocity (from this step's redistribution)
	// Damped by age and explicit damping factor.
	float totalFlow = abs(netFlow);
	if (totalFlow > 0.001 && bloodH > 0.001)
	{
		float2 flowVel = flowDirection / (totalFlow + 0.001);  // normalize by flow magnitude
		flowVel *= totalFlow * rcpCellSize;  // scale to cells/second

		// Blend momentum with new flow direction
		velocity = lerp(flowVel, velocity, saturate(momentumStrength));
	}

	// Damping: velocity decays over time, faster as blood ages
	float dampFactor = exp(-velocityDamping * ageFactor * dt);
	velocity *= dampFactor;

	// Kill velocity when blood is negligible
	if (bloodH < 0.01)
		velocity = 0;

	// === Phase 4: Aging and evaporation ===
	if (bloodH > 0.001)
		bloodAge += dt;

	bloodH = max(0, bloodH - evapRate * dt);

	// === Write outputs ===
	BloodHeightCurr[tc] = float2(bloodH, bloodAge);
	BloodVelocityCurr[tc] = velocity;

	// Stain accumulation: wherever blood sits, permanent stain builds
	float stain = BloodStain[tc];
	if (bloodH > 0.001)
		stain = min(stain + bloodH * dt * 0.5, 1.0);
	BloodStain[tc] = stain;
}
