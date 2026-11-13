#version 450
layout(binding=0) uniform sampler2D tex;
layout(location=0) in vec3 vColor;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec2 vUV;
layout(location=3) in vec3 vWorld;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC {
  mat4 mvp;
  vec4 eye;
  vec4 fog;
  mat4 model;
} pc;
void main(){
  vec3 n = normalize(vNormal);
  vec3 L = normalize(vec3(0.4, -0.6, 0.7));
  float diff = max(dot(n, L), 0.0);
  float light = 0.38 + 0.72 * diff;
  vec3 lit = texture(tex, vUV).rgb * vColor * light;
  float d = distance(vWorld, pc.eye.xyz);
  float f = clamp((d - pc.fog.x) / (pc.fog.y - pc.fog.x), 0.0, 1.0);
  vec3 fogColor = vec3(0.60, 0.68, 0.78);
  outColor = vec4(mix(lit, fogColor, f), 1.0);
}
