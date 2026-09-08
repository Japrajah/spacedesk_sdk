// Reconstructed from the installed UWP shader's reflected layout/instructions.
cbuffer ModelViewProjectionConstantBuffer : register(b0) {
    row_major float4x4 model;
    row_major float4x4 view;
    row_major float4x4 projection;
};
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(Input input) {
    Output output;
    float4 position = mul(float4(input.position, 1), model);
    position = mul(position, view);
    output.position = mul(position, projection);
    output.uv = input.uv;
    return output;
}
