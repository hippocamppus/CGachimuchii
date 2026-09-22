cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;
    float4 gTexTransform;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gsamLinear : register(s0);

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct VertexOut
{
    float4 PosH     : SV_POSITION;
    float2 TexC     : TEXCOORD;
    float3 WorldPos : POSITION1;
    float3 NormalW  : NORMAL1;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float4 positionW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.WorldPos = positionW.xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.TexC = vin.TexC * gTexTransform.xy + gTexTransform.zw;

    return vout;
}

struct GBufferOutput
{
    float4 Albedo        : SV_Target0;
    float4 Normal        : SV_Target1;
    float4 WorldPosition : SV_Target2;
};

GBufferOutput PS(VertexOut pin)
{
    float4 color = gDiffuseMap.Sample(gsamLinear, pin.TexC);
    clip(color.a - 0.5f);

    GBufferOutput output;
    output.Albedo = float4(color.rgb, 1.0f);
    output.Normal = float4(normalize(pin.NormalW), 1.0f);
    output.WorldPosition = float4(pin.WorldPos, 1.0f);
    return output;
}
