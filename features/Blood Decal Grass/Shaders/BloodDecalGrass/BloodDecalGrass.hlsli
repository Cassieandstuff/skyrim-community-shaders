namespace BloodDecalGrass
{
	struct BloodDecalEntry
	{
		float4 positionRadius;   // xyz = world position, w = radius
		float4 colorIntensity;   // xyz = blood color, w = overall intensity
		float4 soakParams;       // x = soak progress (0 = none, 1 = fully soaked), yzw = padding
	};

	StructuredBuffer<BloodDecalEntry> BloodDecals : register(t101);

	// Fluid simulation grid textures
	Texture2D<float2> BloodHeightTex : register(t102);   // R = liquid blood height, G = age
	Texture2D<float> BloodStainTex : register(t103);      // accumulated stain intensity [0,1]
	Texture2D<float> TerrainHeightTex : register(t104);   // terrain elevation for height rejection
	Texture2D<float2> BloodVelocityTex : register(t105);  // RG = flow velocity XY (cells/sec)
	SamplerState BloodGridSampler : register(s7);          // bilinear + wrap (s7 free in Lighting & RunGrass)

	static const uint BLOOD_GRID_DIM = 512;

	// Convert a world XY position to toroidal grid UV for texture sampling.
	// The sampler is set to WRAP mode, so the toroidal addressing is handled
	// automatically — we just need the correct continuous UV.
	float2 WorldToGridUV(float2 worldXY)
	{
		float rcpCellSize = SharedData::bloodDecalGrassSettings.RcpGridCellSize;
		float2 gridWorldOrigin = SharedData::bloodDecalGrassSettings.GridWorldOrigin;
		int2 arrayOrigin = SharedData::bloodDecalGrassSettings.ArrayOrigin;

		// World position to array-space texel coordinate (continuous, not clamped).
		// ArrayOrigin maps local cell 0 to the correct texel in the toroidal buffer.
		float2 uv = ((worldXY - gridWorldOrigin) * rcpCellSize + float2(arrayOrigin)) / float2(BLOOD_GRID_DIM, BLOOD_GRID_DIM);
		return uv;  // WRAP sampler handles toroidal wrapping
	}

	// Height rejection: returns [0,1] factor that fades blood influence for surfaces
	// far above the terrain at that grid cell. Prevents blood from painting walls,
	// objects, and actors that happen to share the same XY as a blood pool.
	float GetHeightRejection(float3 worldPosition)
	{
		if (!SharedData::bloodDecalGrassSettings.EnableFluidSim)
			return 1.0;

		float2 gridUV = WorldToGridUV(worldPosition.xy);
		float terrainH = TerrainHeightTex.SampleLevel(BloodGridSampler, gridUV, 0);
		float heightAbove = worldPosition.z - terrainH;
		float threshold = SharedData::bloodDecalGrassSettings.SurfaceHeightThreshold;

		// Full influence up to 16 units above terrain, fades to 0 over the threshold
		return 1.0 - saturate((heightAbove - 16.0) / max(threshold, 1.0));
	}

	// Fractal Brownian Motion using the existing Perlin noise from Common/Random.hlsli.
	// Returns a value roughly in [-1, 1] with organic, multi-scale detail.
	float FBM(float2 pos, uint seed = 0x578437ADu)
	{
		float value = 0.0;
		float amplitude = 0.5;

		value += amplitude * Random::perlinNoise(float3(pos, 0.0), seed);
		amplitude *= 0.5;
		pos *= 2.03;

		value += amplitude * Random::perlinNoise(float3(pos, 0.0), seed + 1u);
		amplitude *= 0.5;
		pos *= 2.03;

		value += amplitude * Random::perlinNoise(float3(pos, 0.0), seed + 2u);

		return value;
	}

	// Flow-oriented noise: stretches FBM along the velocity direction to create
	// organic rivulet patterns at sub-grid resolution. Where blood flows fast,
	// the noise elongates into narrow streaks. Where blood is still, noise is isotropic.
	float FlowNoise(float2 worldXY, float2 velocity, float flowNoiseScale)
	{
		float speed = length(velocity);
		if (speed < 0.01 || flowNoiseScale < 0.01)
			return 0.0;

		// Flow-aligned coordinate frame
		float2 flowDir = velocity / speed;
		float2 perpDir = float2(-flowDir.y, flowDir.x);

		// Transform world position into flow space
		// Stretch along flow direction (elongates), compress perpendicular (narrows rivulets)
		float stretchFactor = 1.0 + speed * 2.0;
		float compressFactor = 1.0 / max(stretchFactor * 0.5, 1.0);

		float flowCoord = dot(worldXY, flowDir) * compressFactor;
		float perpCoord = dot(worldXY, perpDir) * stretchFactor;

		float2 flowPos = float2(flowCoord, perpCoord) * 0.15;
		float noise = FBM(flowPos, 0xF100Du);

		// Rivulet mask: narrow channels where noise crosses zero
		// Creates thin branching patterns that follow the flow direction
		float rivuletMask = 1.0 - smoothstep(0.0, 0.15, abs(noise));

		// Modulate by speed and scale
		float influence = rivuletMask * saturate(speed * 0.5) * flowNoiseScale;

		return influence;
	}

	// Core soak evaluation shared by both grass and surface paths.
	// flowBias shifts the soak frontier downhill: negative = soak sooner, positive = soak later.
	// Returns a combined influence value before color/intensity scaling.
	float EvaluateSoak(float warpedT, float noiseNorm, float soakProgress, float flowBias)
	{
		float baseInfluence = saturate(1.0 - warpedT);
		baseInfluence *= baseInfluence;

		// effectiveT drives the soak reveal. flowBias stretches it directionally:
		//   downhill pixels get lower effectiveT -> revealed first
		//   uphill pixels get higher effectiveT -> revealed last (or never)
		float effectiveT = warpedT * 0.7 + noiseNorm * 0.3 + flowBias;
		float soakMask = 1.0 - smoothstep(soakProgress - 0.05, soakProgress + 0.05, effectiveT);

		return baseInfluence * soakMask;
	}

	// Grass blood influence — uses Z difference as a gravity proxy since
	// grass doesn't have a surface normal. Blood on a hillside soaks
	// downhill faster than uphill.
	float3 GetBloodInfluence(float3 worldPosition)
	{
		if (!SharedData::bloodDecalGrassSettings.Enabled)
			return 0;

		float bloodIntensity = SharedData::bloodDecalGrassSettings.BloodIntensity;
		uint count = SharedData::bloodDecalGrassSettings.EntryCount;

		float3 bestTint = 0;
		float bestStrength = 0;

		[loop] for (uint i = 0; i < count; i++)
		{
			float3 decalPos = BloodDecals[i].positionRadius.xyz;
			float entryRadius = BloodDecals[i].positionRadius.w;
			float3 decalColor = BloodDecals[i].colorIntensity.xyz;
			float overallIntensity = BloodDecals[i].colorIntensity.w;
			float soakProgress = BloodDecals[i].soakParams.x;

			float invRadius = 1.0 / max(entryRadius, 1.0);

			float dist = distance(worldPosition.xy, decalPos.xy);
			float t = dist * invRadius;

			float noiseScale = 0.08 / max(invRadius, 0.001);
			float noise = FBM(worldPosition.xy / max(noiseScale, 1.0), i * 7u + 0xB100Du);

			float warpStrength = 0.45;
			float warpedT = t + noise * warpStrength * smoothstep(0.0, 0.7, t);

			float noiseNorm = noise * 0.5 + 0.5;

			// Gravity bias from Z difference: grass below the source soaks first.
			// Normalized by radius so the effect scales consistently.
			float zDiff = worldPosition.z - decalPos.z;
			float flowBias = (zDiff * invRadius) * 0.3;

			float influence = EvaluateSoak(warpedT, noiseNorm, soakProgress, flowBias);
			influence *= overallIntensity * bloodIntensity;

			if (influence > bestStrength) {
				bestStrength = influence;
				bestTint = decalColor;
			}
		}

		// Fluid simulation stain overlay: wherever the sim deposited blood,
		// layer it on top of (or instead of) the per-source noise pattern.
		if (SharedData::bloodDecalGrassSettings.EnableFluidSim)
		{
			float heightFade = GetHeightRejection(worldPosition);
			if (heightFade > 0.01)
			{
				float2 gridUV = WorldToGridUV(worldPosition.xy);
				float stain = BloodStainTex.SampleLevel(BloodGridSampler, gridUV, 0);
				float2 bloodData = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV, 0);
				float liquidBlood = bloodData.x;

				// Flow noise: adds rivulet detail at sub-grid resolution
				float2 velocity = BloodVelocityTex.SampleLevel(BloodGridSampler, gridUV, 0);
				float flowNoiseScale = SharedData::bloodDecalGrassSettings.FlowNoiseScale;
				float rivuletDetail = FlowNoise(worldPosition.xy, velocity, flowNoiseScale);

				// Liquid blood contributes more strongly than dried stain
				float fluidInfluence = saturate(stain + liquidBlood * 2.0 + rivuletDetail) * bloodIntensity * heightFade;

				if (fluidInfluence > bestStrength)
				{
					bestStrength = fluidInfluence;
					// Use nearest source color if available, otherwise use the configured blood color
					if (dot(bestTint, 1) < 0.001)
						bestTint = float3(SharedData::bloodDecalGrassSettings.BloodColorR,
						                  SharedData::bloodDecalGrassSettings.BloodColorG,
						                  SharedData::bloodDecalGrassSettings.BloodColorB);
				}
			}
		}

		return bestTint * bestStrength;
	}

	// Parallax ray-march: offsets the sampling UV by tracing a ray through
	// the blood height field. Creates apparent depth for pools without geometry.
	// The grid is axis-aligned so tangent space is trivially world XY/Z.
	float2 ParallaxOffset(float2 gridUV, float3 viewDir, float depthScale)
	{
		// View direction in grid-tangent space: XY is horizontal, Z is up.
		// We need the horizontal component relative to the vertical.
		float2 viewXY = viewDir.xy;
		float viewZ = abs(viewDir.z) + 0.001;  // avoid division by zero at grazing
		float2 parallaxDir = viewXY / viewZ;

		// Scale by depth and convert to UV space
		float rcpGridDim = 1.0 / float(BLOOD_GRID_DIM);
		float rcpCellSize = SharedData::bloodDecalGrassSettings.RcpGridCellSize;
		float2 maxOffset = parallaxDir * depthScale * rcpCellSize * rcpGridDim;

		// 6-step linear search
		static const int NUM_STEPS = 6;
		float stepSize = 1.0 / float(NUM_STEPS);

		float2 uvStep = maxOffset * stepSize;
		float2 currUV = gridUV;
		float currHeight = 0.0;
		float prevHeight = 0.0;
		float currLayerDepth = 0.0;

		[unroll]
		for (int s = 0; s < NUM_STEPS; s++)
		{
			currUV -= uvStep;
			currLayerDepth += stepSize;

			float bloodH = BloodHeightTex.SampleLevel(BloodGridSampler, currUV, 0).x;
			// Normalize blood height relative to depth scale
			float normalizedH = saturate(bloodH / max(depthScale * 0.5, 0.01));

			prevHeight = currHeight;
			currHeight = normalizedH;

			if (currHeight >= currLayerDepth)
				break;
		}

		// Secant refinement: interpolate between last two steps
		float prevLayerDepth = currLayerDepth - stepSize;
		float afterDepth = currHeight - currLayerDepth;
		float beforeDepth = prevHeight - prevLayerDepth;
		float weight = afterDepth / (afterDepth - beforeDepth + 0.001);

		float2 finalUV = currUV + uvStep * weight;

		return finalUV;
	}

	// Surface-aware blood influence for non-grass meshes (Lighting shader).
	// Uses the full gravity-projected flow direction to bias the soak pattern.
	// On flat ground, blood pools uniformly. On slopes, it elongates downhill.
	// On steep walls, it becomes a narrow downward streak — pooling and dripping
	// are the same effect at different surface angles.
	float3 GetSurfaceBloodInfluence(float3 worldPosition, float3 worldNormal, float3 viewDirection)
	{
		if (!SharedData::bloodDecalGrassSettings.EnableSurfaceStaining)
			return 0;

		float bloodIntensity = SharedData::bloodDecalGrassSettings.BloodIntensity;
		float heightThreshold = SharedData::bloodDecalGrassSettings.SurfaceHeightThreshold;
		float normalThreshold = SharedData::bloodDecalGrassSettings.SurfaceNormalThreshold;
		float dripReachMul = SharedData::bloodDecalGrassSettings.DripReachMultiplier;

		// Gravity projected onto this surface — the direction blood flows here.
		// flowMag is 0 on flat ground, ~1 on a vertical wall.
		static const float3 gravity = float3(0, 0, -1);
		float3 flowDir = gravity - dot(gravity, worldNormal) * worldNormal;
		float flowMag = length(flowDir);
		flowDir = flowMag > 0.001 ? flowDir / flowMag : 0;

		// How upward the surface faces (1 = flat ground, 0 = vertical wall)
		float upDot = worldNormal.z;

		float3 bestTint = 0;
		float bestStrength = 0;

		uint count = SharedData::bloodDecalGrassSettings.EntryCount;

		[loop] for (uint i = 0; i < count; i++)
		{
			float3 decalPos = BloodDecals[i].positionRadius.xyz;
			float entryRadius = BloodDecals[i].positionRadius.w;
			float3 decalColor = BloodDecals[i].colorIntensity.xyz;
			float overallIntensity = BloodDecals[i].colorIntensity.w;
			float soakProgress = BloodDecals[i].soakParams.x;

			float3 toPixel = worldPosition - decalPos;

			// Effective radius stretches downhill based on slope and reach multiplier.
			// Flat surfaces keep their normal radius. Steep surfaces extend further
			// in the flow direction, allowing drips to travel beyond the base radius.
			float effectiveRadius = entryRadius * lerp(1.0, dripReachMul, flowMag);

			float dist3D = length(toPixel);
			if (dist3D > effectiveRadius)
				continue;

			// Height check for near-horizontal surfaces
			float heightDiff = abs(toPixel.z);
			if (upDot > normalThreshold && heightDiff > heightThreshold)
				continue;

			float invRadius = 1.0 / max(effectiveRadius, 1.0);
			float t = dist3D * invRadius;

			float noiseScale = 0.08 / max(invRadius, 0.001);
			float noise = FBM(worldPosition.xy / max(noiseScale, 1.0), i * 7u + 0xB100Du);

			float warpStrength = 0.35;
			float warpedT = t + noise * warpStrength * smoothstep(0.0, 0.7, t);

			float noiseNorm = noise * 0.5 + 0.5;

			// Flow bias: how far uphill/downhill is this pixel from the source?
			// downhillDist is positive when pixel is downhill from source.
			// The bias makes downhill pixels soak first, uphill pixels soak last.
			// Scale by flowMag so flat surfaces have zero bias (uniform soak).
			float downhillDist = dot(toPixel, flowDir);
			float flowBias = (-downhillDist * invRadius) * flowMag * 0.5;

			float influence = EvaluateSoak(warpedT, noiseNorm, soakProgress, flowBias);

			// Normal-based weighting: upward surfaces get full strength,
			// vertical/overhang surfaces get progressively weaker unless
			// they're receiving flow from above.
			float normalWeight = saturate(upDot * 0.5 + 0.5 + flowMag * 0.3);

			// Height falloff for upward-facing surfaces
			float heightFactor = upDot > normalThreshold
				? 1.0 - smoothstep(0.0, heightThreshold, heightDiff)
				: 1.0;

			influence *= normalWeight * heightFactor * overallIntensity * bloodIntensity;

			if (influence > bestStrength) {
				bestStrength = influence;
				bestTint = decalColor;
			}
		}

		return bestTint * bestStrength;
	}

	// Backward-compatible overload for callsites that don't pass viewDirection.
	float3 GetSurfaceBloodInfluence(float3 worldPosition, float3 worldNormal)
	{
		return GetSurfaceBloodInfluence(worldPosition, worldNormal, float3(0, 0, 1));
	}

	// Standalone fluid simulation influence for surfaces.
	// Returns (blood tint * fluid strength), gated on EnableFluidSim.
	// Independent of the noise-soak system — call both and pick the stronger result.
	// Tint is always the user-configured blood colour (no per-decal override).
	float3 GetFluidSurfaceInfluence(float3 worldPosition, float3 worldNormal, float3 viewDirection)
	{
		if (!SharedData::bloodDecalGrassSettings.EnableFluidSim)
			return 0;

		float heightFade = GetHeightRejection(worldPosition);
		if (heightFade <= 0.01)
			return 0;

		float bloodIntensity = SharedData::bloodDecalGrassSettings.BloodIntensity;
		float2 gridUV = WorldToGridUV(worldPosition.xy);

		if (SharedData::bloodDecalGrassSettings.EnableParallax)
		{
			float depthScale = SharedData::bloodDecalGrassSettings.ParallaxDepthScale;
			gridUV = ParallaxOffset(gridUV, viewDirection, depthScale);
		}

		float stain = BloodStainTex.SampleLevel(BloodGridSampler, gridUV, 0);
		float2 bloodData = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV, 0);
		float liquidBlood = bloodData.x;

		float2 velocity = BloodVelocityTex.SampleLevel(BloodGridSampler, gridUV, 0);
		float flowNoiseScale = SharedData::bloodDecalGrassSettings.FlowNoiseScale;
		float rivuletDetail = FlowNoise(worldPosition.xy, velocity, flowNoiseScale);

		float fluidInfluence = saturate(stain + liquidBlood * 2.0 + rivuletDetail) * bloodIntensity * heightFade;

		float3 tint = float3(SharedData::bloodDecalGrassSettings.BloodColorR,
		                     SharedData::bloodDecalGrassSettings.BloodColorG,
		                     SharedData::bloodDecalGrassSettings.BloodColorB);
		return tint * fluidInfluence;
	}

	// Overload without viewDirection — defaults to straight up (no parallax shift).
	float3 GetFluidSurfaceInfluence(float3 worldPosition, float3 worldNormal)
	{
		return GetFluidSurfaceInfluence(worldPosition, worldNormal, float3(0, 0, 1));
	}

	// Surface properties derived from the fluid simulation height map.
	// Computes normal perturbation from blood surface curvature and
	// age-dependent roughness for wet/coagulated blood appearance.
	struct BloodSurfaceProperties
	{
		float3 normal;      // world-space normal blended toward blood surface
		float roughness;    // glossy for fresh liquid, matte for coagulated/stain
		float wetness;      // [0,1] how liquid the blood is (drives Fresnel + darkening)
	};

	BloodSurfaceProperties GetBloodSurfaceProperties(float3 worldPosition, float3 worldNormal, float bloodStrength, float3 viewDirection)
	{
		BloodSurfaceProperties result;
		result.normal = worldNormal;
		result.roughness = 1.0;
		result.wetness = 0.0;

		if (!SharedData::bloodDecalGrassSettings.EnableFluidSim || bloodStrength < 0.01)
			return result;

		// Height rejection: don't compute surface properties for pixels far above terrain
		float heightFade = GetHeightRejection(worldPosition);
		if (heightFade < 0.01)
			return result;

		float2 gridUV = WorldToGridUV(worldPosition.xy);

		// Parallax: offset the UV by ray-marching the height field
		if (SharedData::bloodDecalGrassSettings.EnableParallax)
		{
			float depthScale = SharedData::bloodDecalGrassSettings.ParallaxDepthScale;
			gridUV = ParallaxOffset(gridUV, viewDirection, depthScale);
		}

		float texelSize = 1.0 / float(BLOOD_GRID_DIM);

		// Sample blood height at pixel and 4 neighbors for gradient
		float2 centerData = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV, 0);
		float hCenter = centerData.x;
		float age = centerData.y;

		float stain = BloodStainTex.SampleLevel(BloodGridSampler, gridUV, 0);
		float totalPresence = saturate(hCenter * 2.0 + stain);

		if (totalPresence < 0.01)
			return result;

		// Compute gradient from height map neighbors
		float hRight = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV + float2(texelSize, 0), 0).x;
		float hLeft  = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV - float2(texelSize, 0), 0).x;
		float hUp    = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV + float2(0, texelSize), 0).x;
		float hDown  = BloodHeightTex.SampleLevel(BloodGridSampler, gridUV - float2(0, texelSize), 0).x;

		float cellSize = SharedData::bloodDecalGrassSettings.GridCellSize;
		float dhdx = (hRight - hLeft) / (2.0 * cellSize);
		float dhdy = (hUp - hDown) / (2.0 * cellSize);

		// Blood surface normal: mostly up (Z), tilted by height gradient.
		// Amplify gradient for visible perturbation — raw height differences
		// are tiny (sub-unit) so we scale up for visual impact.
		float normalStrength = 40.0;
		float3 bloodNormal = normalize(float3(-dhdx * normalStrength, -dhdy * normalStrength, 1.0));

		// Blend between original surface normal and blood surface normal.
		// Liquid blood fully overrides, dried stain gets slight perturbation.
		float liquidPresence = saturate(hCenter * 4.0);
		result.normal = normalize(lerp(worldNormal, bloodNormal, liquidPresence * 0.8));

		// Wetness: fresh liquid blood is very wet, dried stain is dry
		result.wetness = liquidPresence;

		// Roughness: fresh blood is glossy (0.05), coagulated is matte (0.5).
		// Age drives coagulation; stain-only areas are fully matte.
		float ageNorm = saturate(age * 0.05);  // age ~20s = fully coagulated
		float wetRoughness = lerp(0.05, 0.5, ageNorm);
		float stainRoughness = 0.6;
		result.roughness = lerp(stainRoughness, wetRoughness, liquidPresence);

		return result;
	}

	// Backward-compatible overload without viewDirection
	BloodSurfaceProperties GetBloodSurfaceProperties(float3 worldPosition, float3 worldNormal, float bloodStrength)
	{
		return GetBloodSurfaceProperties(worldPosition, worldNormal, bloodStrength, float3(0, 0, 1));
	}
}
