#version 450
layout(binding=0) uniform sampler2D tex0;
layout(binding=1) uniform sampler2D tex1;
layout(binding=2) uniform sampler2D tex2;
layout(location=0) in vec3 vWeight;
layout(location=1) in vec3 vTint;
layout(location=2) in vec3 vNormal;
layout(location=3) in vec2 vUV0;
layout(location=4) in vec2 vUV1;
layout(location=5) in vec2 vUV2;
layout(location=6) in vec3 vWorld;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC {
  mat4 mvp;
  vec4 eye;
  vec4 fog;
} pc;
void main(){
  vec3 n = normalize(vNormal);
  vec3 L = normalize(vec3(0.4, -0.6, 0.7));
  float diff = max(dot(n, L), 0.0);
  float light = 0.38 + 0.72 * diff;
  vec3 base = texture(tex0, vUV0).rgb * vWeight.r
            + texture(tex1, vUV1).rgb * vWeight.g
            + texture(tex2, vUV2).rgb * vWeight.b;
  vec3 lit = base * vTint * light;
  float d = distance(vWorld, pc.eye.xyz);
  float f = clamp((d - pc.fog.x) / (pc.fog.y - pc.fog.x), 0.0, 1.0);
  vec3 fogColor = vec3(0.60, 0.68, 0.78);
  outColor = vec4(mix(lit, fogColor, f), 1.0);
}
