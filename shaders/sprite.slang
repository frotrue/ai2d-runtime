struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 tint : COLOR0;
};

struct InstanceInput
{
    [[vk::location(0)]] float2 position : POSITION0;
    [[vk::location(1)]] float2 size : TEXCOORD0;
    [[vk::location(2)]] float rotation : TEXCOORD1;
    [[vk::location(3)]] float4 uv_rect : TEXCOORD2;
    [[vk::location(4)]] float4 tint : COLOR0;
    [[vk::location(5)]] float2 pivot : TEXCOORD3;
};

[shader("vertex")]
VertexOutput vertex_main(InstanceInput instance, uint vertex_id : SV_VertexID)
{
    const float2 unit_uv[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0)
    };
    const float sine_value = sin(instance.rotation);
    const float cosine_value = cos(instance.rotation);
    const float2 local = (unit_uv[vertex_id] - instance.pivot) * instance.size;
    const float2 rotated = float2(
        local.x * cosine_value - local.y * sine_value,
        local.x * sine_value + local.y * cosine_value);

    VertexOutput output;
    output.position = float4(instance.position + rotated, 0.0, 1.0);
    output.uv = instance.uv_rect.xy + unit_uv[vertex_id] * instance.uv_rect.zw;
    output.tint = instance.tint;
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> sprite_texture;
[[vk::binding(1, 0)]] SamplerState sprite_sampler;

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return sprite_texture.Sample(sprite_sampler, input.uv) * input.tint;
}
