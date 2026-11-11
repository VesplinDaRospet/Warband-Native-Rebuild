#version 450
layout(binding=0) uniform sampler2D tex;
layout(location=0) in vec3 vColor;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec2 vUV;
layout(location=0) out vec4 outColor;
void main(){
  vec3 n = normalize(vNormal);
  vec3 L = normalize(vec3(0.4, -0.6, 0.7));
  float diff = max(dot(n, L), 0.0);
  float light = 0.38 + 0.72 * diff;
  outColor = texture(tex, vUV) * vec4(vColor * light, 1.0);
}
