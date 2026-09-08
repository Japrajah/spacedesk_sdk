// Native input texture is R8G8B8A8 containing BGRA bytes. UWP swaps R/B here.
Texture2D txDiffuse : register(t0);
SamplerState samLinear : register(s0);
struct Input { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(Input input) : SV_TARGET { return txDiffuse.Sample(samLinear, input.uv).bgra; }
