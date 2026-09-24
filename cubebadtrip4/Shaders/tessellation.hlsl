cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gViewProj;
    float4x4 gWorld;

    float3 gEyePosW;
    float gDisplacementScale;

    float4 gTessParams;
    float4 gTexTransform;
};

Texture2D gBaseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);

SamplerState gsamLinear : register(s0);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct PatchConstants
{
    float Edges[3] : SV_TessFactor;
    float Inside : SV_InsideTessFactor;
};

struct DomainOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION1;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    vout.PosL = vin.PosL;
    vout.NormalL = vin.NormalL;
    vout.TexC = vin.TexC;

    return vout;
}

float GetTessellationFactor(float3 p0, float3 p1, float3 p2)
{
    float3 centerL = (p0 + p1 + p2) / 3.0f;
    float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;

    float distanceToCamera = distance(centerW, gEyePosW);
    float minDistance = gTessParams.x;
    float maxDistance = max(gTessParams.y, minDistance + 0.001f);
    float minFactor = max(gTessParams.z, 1.0f);
    float maxFactor = max(gTessParams.w, minFactor);

    float amount = saturate(
        (maxDistance - distanceToCamera) /
        (maxDistance - minDistance));

    return lerp(minFactor, maxFactor, amount);
}

PatchConstants PatchHS(
    InputPatch<VertexOut, 3> patch,
    uint patchId : SV_PrimitiveID)
{
    PatchConstants output;

    float factor = GetTessellationFactor(
        patch[0].PosL,
        patch[1].PosL,
        patch[2].PosL);

    output.Edges[0] = factor;
    output.Edges[1] = factor;
    output.Edges[2] = factor;
    output.Inside = factor;

    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchHS")]
VertexOut HS(
    InputPatch<VertexOut, 3> patch,
    uint controlPointId : SV_OutputControlPointID,
    uint patchId : SV_PrimitiveID)
{
    return patch[controlPointId];
}

[domain("tri")]
DomainOut DS(
    PatchConstants factors,
    float3 barycentric : SV_DomainLocation,
    const OutputPatch<VertexOut, 3> patch)
{
    DomainOut output;

    float3 positionL =
        patch[0].PosL * barycentric.x +
        patch[1].PosL * barycentric.y +
        patch[2].PosL * barycentric.z;

    float3 normalL = normalize(
        patch[0].NormalL * barycentric.x +
        patch[1].NormalL * barycentric.y +
        patch[2].NormalL * barycentric.z);

    float2 texC =
        patch[0].TexC * barycentric.x +
        patch[1].TexC * barycentric.y +
        patch[2].TexC * barycentric.z;

    float2 displacementTexC =
        texC * gTexTransform.xy +
        gTexTransform.zw;

   float displacement =
    gDisplacementMap.SampleLevel(
        gsamLinear,
        displacementTexC,
        0.0f
    ).r;

displacement =
    saturate(
        (displacement - 0.5f) * 1.4f + 0.5f
    );

displacement =
    (displacement - 0.5f) * gDisplacementScale;

    float3 positionW =
        mul(float4(positionL, 1.0f), gWorld).xyz;

    float3 normalW = normalize(
        mul(float4(normalL, 0.0f), gWorld).xyz);

    positionW += normalW * displacement;

    output.PosW = positionW;
    output.NormalW = normalW;
    output.TexC = texC;
    output.PosH =
        mul(float4(positionW, 1.0f), gViewProj);

    return output;
}

float4 PS(DomainOut pin) : SV_Target
{
    float2 texC =
        pin.TexC * gTexTransform.xy +
        gTexTransform.zw;

    float4 baseColor =
        gBaseMap.Sample(gsamLinear, texC);

    clip(baseColor.a - 0.5f);

    float3 normalW = normalize(pin.NormalW);

    float3 dp1 = ddx(pin.PosW);
    float3 dp2 = ddy(pin.PosW);
    float2 duv1 = ddx(pin.TexC);
    float2 duv2 = ddy(pin.TexC);

    float3 tangentW = normalize(
        dp1 * duv2.y - dp2 * duv1.y);

    tangentW = normalize(
        tangentW -
        normalW * dot(tangentW, normalW));

    float3 bitangentW = normalize(
        cross(normalW, tangentW));

    float3 normalSample =
    gNormalMap.Sample(gsamLinear, texC).xyz * 2.0f - 1.0f;

normalSample.xy *= 0.25f;

normalSample.z =
    sqrt(
        saturate(
            1.0f -
            dot(normalSample.xy, normalSample.xy)
        )
    );

float3 mappedNormalW =
    normalize(
        normalSample.x * tangentW +
        normalSample.y * bitangentW +
        normalSample.z * normalW
    );

    float3 lightDirection =
        normalize(float3(-0.35f, 0.8f, -0.45f));

    float diffuse =
        saturate(dot(mappedNormalW, lightDirection));

    float3 ambient =
    float3(0.4f, 0.4f, 0.4f);

float3 color =
    baseColor.rgb *
    (ambient + diffuse * float3(0.8f, 0.8f, 0.8f));

color =
    saturate(color * 1.15f);

return float4(color, baseColor.a);
}
