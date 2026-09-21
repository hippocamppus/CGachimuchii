cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
};

Texture2D gDiffuseMap : register(t0);

SamplerState gsamLinear : register(s0);

struct VertexIn
{
    float3 Pos    : POSITION;
    float3 Normal : NORMAL;
    float2 TexC   : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    vout.PosH = mul(
        float4(vin.Pos, 1.0f),
        gWorldViewProj);

    vout.TexC = vin.TexC;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 color =
        gDiffuseMap.Sample(
            gsamLinear,
            pin.TexC);

    clip(color.a - 0.5f);

    return color;
}