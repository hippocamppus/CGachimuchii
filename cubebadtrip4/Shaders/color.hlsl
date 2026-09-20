cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
};

struct VertexIn
{
    float3 PosL   : POSITION;
    float3 Normal : NORMAL;
    float2 TexC   : TEXCOORD;
};

struct VertexOut
{
    float4 PosH   : SV_POSITION;
    float2 TexC   : TEXCOORD;
    float3 Normal : NORMAL;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    vout.PosH =
        mul(
            float4(vin.PosL, 1.0f),
            gWorldViewProj
        );

    vout.TexC =
        vin.TexC;

    vout.Normal =
        vin.Normal;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{

    return float4(
        frac(pin.TexC),
        0.0f,
        1.0f
    );
}