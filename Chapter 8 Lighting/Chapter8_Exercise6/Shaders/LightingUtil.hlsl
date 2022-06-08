//***************************************************************************************
// LightingUtil.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Contains API for shader lighting.
//***************************************************************************************

#define MaxLights 16

struct Light
{
	float3 Strength;
	float  FalloffStart; // point/spot light only
	float3 Direction;    // directional/spot light only. Direction the light ray travels
	float  FalloffEnd;   // point/spot light only
	float3 Position;     // point light only
	float  SpotPower;    // spot light only
};

struct Material
{
	float4 DiffuseAlbedo;
	float3 FresnelR0;

	// shininess is inverse of roughness: shininess = 1 - roughness
	float Shininess;
};

// a linear falloff function that applies to point lights and spot lights:  page 340, figure 8.26
float CalcAttenuation(float d, float falloffStart, float falloffEnd)
{
	// Linear falloff.
	return saturate((falloffEnd - d) / (falloffEnd - falloffStart));
}

// Schlick gives an approximation to Fresnel reflectance (see pg. 233 "Real-Time Rendering 3rd Ed.").
// R0 = ( (n-1)/(n+1) )^2, where n is the index of refraction.
// page 328
float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
	float  cosIncidentAngle = saturate(dot(normal, lightVec));
	float  f0               = 1.0f - cosIncidentAngle;
	float3 reflectPercent   = R0 + (1.0f - R0) * (f0 * f0 * f0 * f0 * f0);

	return reflectPercent;
}

// see Notability
float3 BlinnPhong(float3 lightStrength, float3 lightVec, float3 normal, float3 toEye, Material mat)
{
	// derive m from the shininess
	// mat.Shininess is [0, 1], thus m is [0, 256]. See page 332, figure 8.20
	const float m       = mat.Shininess * 256.0f;
	float3      halfVec = normalize(toEye + lightVec);

	float roughnessFactor = (m + 8.0f) * pow(max(dot(halfVec, normal), 0.0f), m) / 8.0f;

	//!? look here: toon shading
	if(roughnessFactor <= 0.1f)
	{
        roughnessFactor = 0.0f;
    }
    else if (roughnessFactor <= 0.5f)
    {
        roughnessFactor = 0.5f;
    }
	else
	{
        roughnessFactor = 0.8f;
    }

	// Note: using micro-facet model, the normal vector in Schlick approximation is the half vector
	float3 fresnelFactor = SchlickFresnel(mat.FresnelR0, halfVec, lightVec);

	float3 specAlbedo = fresnelFactor * roughnessFactor;

	// Our spec formula goes outside [0,1] range, but we are 
	// doing LDR rendering.  So scale it down a bit. https://www.desmos.com/calculator/ii1ypr57sl
	// TODO: HDR see page 345
	specAlbedo = specAlbedo / (specAlbedo + 1.0f);

	return (mat.DiffuseAlbedo.rgb + specAlbedo) * lightStrength;
}

//---------------------------------------------------------------------------------------
// Evaluates the lighting equation for directional lights.
//---------------------------------------------------------------------------------------
float3 ComputeDirectionalLight(Light L, Material mat, float3 normal, float3 toEye)
{
	// The directional light vector aims opposite the direction the light rays travel.
	float3 lightVec = -L.Direction;

	// directional light strength is modulated by cosine law 

	// Scale light down by Lambert's cosine law.
	float ndotl = max(dot(lightVec, normal), 0.0f);

	//!? look here: toon shading
	if (ndotl <= 0.0f)
	{
		ndotl = 0.4f;
	}
	else if (ndotl <= 0.5f)
	{
		ndotl = 0.6f;
	}
	else
	{
		ndotl = 1.0f;
	}

	float3 lightStrength = L.Strength * ndotl;

	return BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
}

//---------------------------------------------------------------------------------------
// Evaluates the lighting equation for point lights.
//---------------------------------------------------------------------------------------
float3 ComputePointLight(Light L, Material mat, float3 pos, float3 normal, float3 toEye)
{
	// The point light vector aims from the surface point to the light source
	float3 lightVec = L.Position - pos;

	// point light strength is modulated by cosine law and attenuation factor 

	// The distance from surface to light.
	float d = length(lightVec);

	// Range test.
	if (d > L.FalloffEnd)
		return 0.0f; // a point whose distance from the light source is >= falloffEnd receives no light

	// Normalize the light vector.
	lightVec /= d;

	// Scale light down by Lambert's cosine law.
	float  ndotl         = max(dot(lightVec, normal), 0.0f);
	float3 lightStrength = L.Strength * ndotl;

	// Attenuate light by distance.
	float att = CalcAttenuation(d, L.FalloffStart, L.FalloffEnd);
	lightStrength *= att;

	return BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
}

//---------------------------------------------------------------------------------------
// Evaluates the lighting equation for spot lights.
//---------------------------------------------------------------------------------------
float3 ComputeSpotLight(Light L, Material mat, float3 pos, float3 normal, float3 toEye)
{
	//  The spot light vector aims from the surface point to the light source (same as point light)
	float3 lightVec = L.Position - pos;

	// spot light strength is modulated by cosine law and attenuation factor,
	// and spotlight factor that scale the light intensity based on where the point is with respect to the spot light cone

	// The distance from surface to light.
	float d = length(lightVec);

	// Range test.
	if (d > L.FalloffEnd)
		return 0.0f;

	// Normalize the light vector.
	lightVec /= d;

	// Scale light down by Lambert's cosine law.
	float  ndotl         = max(dot(lightVec, normal), 0.0f);
	float3 lightStrength = L.Strength * ndotl;

	// Attenuate light by distance.
	float att = CalcAttenuation(d, L.FalloffStart, L.FalloffEnd);
	lightStrength *= att;

	// Scale by spotlight
	float spotFactor = pow(max(dot(-lightVec, L.Direction), 0.0f), L.SpotPower);
	lightStrength *= spotFactor;

	return BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
}

float4 ComputeLighting(Light    gLights[MaxLights],
                       Material mat,
                       float3   pos,
                       float3   normal,
                       float3   toEye,
                       float3   shadowFactor)
{
	float3 result = 0.0f;

	int i = 0;

	#if (NUM_DIR_LIGHTS > 0)
	for (i = 0; i < NUM_DIR_LIGHTS; ++i)
	{
		result += shadowFactor[i] * ComputeDirectionalLight(gLights[i], mat, normal, toEye);
	}
	#endif

	#if (NUM_POINT_LIGHTS > 0)
    for(i = NUM_DIR_LIGHTS; i < NUM_DIR_LIGHTS+NUM_POINT_LIGHTS; ++i)
    {
        result += ComputePointLight(gLights[i], mat, pos, normal, toEye);
    }
	#endif

	#if (NUM_SPOT_LIGHTS > 0)
    for(i = NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS; ++i)
    {
        result += ComputeSpotLight(gLights[i], mat, pos, normal, toEye);
    }
	#endif

	return float4(result, 0.0f);
}