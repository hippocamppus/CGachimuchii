Texture2D gAlbedoTex : register(t0);
Texture2D gNormalTex : register(t1);
Texture2D gDepthTex : register(t2);
SamplerState gLinearClamp : register(s0);

cbuffer PassConstants : register(b0)
{
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float gPassPadding;
    float4 gAmbientColor;
};

struct LightData
{
    float3 Position;
    float Range;
    float3 Direction;
    float SpotAngle;
    float3 Color;
    float Intensity;
    uint Type;
    float3 Padding;
};

cbuffer LightConstants : register(b1)
{
    LightData gLight;
    uint gEnableAmbient;
    float3 gDummy;
};

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
    VSOut vout;
    float2 pos[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    float2 uv[3] = { float2(0, 1), float2(0, -1), float2(2, 1) };
    vout.PosH = float4(pos[vid], 0, 1);
    vout.TexC = uv[vid];
    return vout;
}

float3 ReconstructWorldPos(float2 uv, float depth, float4x4 invViewProj)
{
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float4 clip = float4(ndc, depth, 1.0f);
    float4 world = mul(clip, invViewProj);
    return world.xyz / world.w;
}

float3 ComputeDirectional(float3 N, float3 albedo, LightData light)
{
    float3 L = normalize(-light.Direction);
    float ndotl = saturate(dot(N, L));
    return albedo * light.Color * (light.Intensity * ndotl);
}

float3 ComputePoint(float3 P, float3 N, float3 albedo, LightData light)
{
    float3 toLight = light.Position - P;
    float dist = length(toLight);
    if (dist > light.Range)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 L = toLight / max(dist, 1e-4f);
    float ndotl = saturate(dot(N, L));
    float atten = saturate(1.0f - dist / max(light.Range, 1e-4f));
    atten *= atten;

    return albedo * light.Color * (light.Intensity * ndotl * atten);
}

float3 ComputeSpot(float3 P, float3 N, float3 albedo, LightData light)
{
    float3 toLight = light.Position - P;
    float dist = length(toLight);
    if (dist > light.Range)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 L = toLight / max(dist, 1e-4f);
    float cone = dot(normalize(-light.Direction), L);
    float spot = smoothstep(light.SpotAngle, light.SpotAngle + 0.08f, cone);

    float ndotl = saturate(dot(N, L));
    float atten = saturate(1.0f - dist / max(light.Range, 1e-4f));
    atten *= atten;

    return albedo * light.Color * (light.Intensity * ndotl * atten * spot);
}

float4 PS_Lighting(VSOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float3 albedo = gAlbedoTex.Sample(gLinearClamp, uv).rgb;
    float3 normalPacked = gNormalTex.Sample(gLinearClamp, uv).rgb;
    float depth = gDepthTex.Sample(gLinearClamp, uv).r;

    float3 normalW = normalize(normalPacked * 2.0f - 1.0f);
    float3 worldPos = ReconstructWorldPos(uv, depth, gInvViewProj);

    float3 color = float3(0.0f, 0.0f, 0.0f);

    if (gEnableAmbient != 0)
    {
        color = gAmbientColor.rgb * albedo;
    }
    else
    {
        if (gLight.Type == 0)
        {
            color = ComputeDirectional(normalW, albedo, gLight);
        }
        else if (gLight.Type == 1)
        {
            color = ComputePoint(worldPos, normalW, albedo, gLight);
        }
        else if (gLight.Type == 2)
        {
            color = ComputeSpot(worldPos, normalW, albedo, gLight);
        }
    }

    return float4(color, 0.0f);
}


