Texture2D<float4> gAlbedoTexture        : register(t0);
Texture2D<float4> gNormalTexture        : register(t1);
Texture2D<float4> gWorldPositionTexture : register(t2);

cbuffer cbLight : register(b0)
{
    float4 gPositionRange;
    float4 gDirectionIntensity;
    float4 gColorType;
    float4 gSpotParams;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

VertexOut VS(uint vertexId : SV_VertexID)
{
    VertexOut output;

    // One oversized triangle covers the complete screen.
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.PosH = float4(
        uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        0.0f,
        1.0f);

    return output;
}

float4 PS(VertexOut pin) : SV_Target
{
    int2 pixelPosition = int2(pin.PosH.xy);

    float4 albedo = gAlbedoTexture.Load(int3(pixelPosition, 0));
    if (albedo.a < 0.5f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Type 3 is the separate ambient pass. It runs once, before direct lights.
    if (gColorType.w > 2.5f)
    {
        return float4(
            albedo.rgb * gDirectionIntensity.w,
            1.0f);
    }

    float3 normal = normalize(
        gNormalTexture.Load(int3(pixelPosition, 0)).xyz);
    float3 worldPosition =
        gWorldPositionTexture.Load(int3(pixelPosition, 0)).xyz;

    float3 lightDirection;
    float attenuation = 1.0f;

    if (gColorType.w < 0.5f)
    {
        // Directional light direction points from the light into the scene.
        lightDirection = normalize(-gDirectionIntensity.xyz);
    }
    else
    {
        float3 toLight = gPositionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        float range = max(gPositionRange.w, 0.001f);

        if (distanceToLight >= range)
        {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        lightDirection = toLight / max(distanceToLight, 0.001f);
        float rangeFade = saturate(1.0f - distanceToLight / range);
        attenuation = rangeFade * rangeFade;

        if (gColorType.w > 1.5f)
        {
            // Spot direction points outward from the light.
            float3 lightToPixel = normalize(worldPosition - gPositionRange.xyz);
            float coneAmount = dot(
                normalize(gDirectionIntensity.xyz),
                lightToPixel);
            attenuation *= smoothstep(
                gSpotParams.y,
                gSpotParams.x,
                coneAmount);
        }
    }

    float diffuse = saturate(dot(normal, lightDirection));
    float intensity = gDirectionIntensity.w;
    float3 result = albedo.rgb
        * gColorType.rgb
        * intensity
        * attenuation
        * diffuse;

    return float4(result, 1.0f);
}
