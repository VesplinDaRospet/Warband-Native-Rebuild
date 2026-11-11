#version 450
layout(binding=0) uniform sampler2D tex;
layout(location=0) in vec3 vColor;
layout(location=1) in vec2 vUV;
layout(location=0) out vec4 outColor;
void main(){
  outColor = texture(tex, vUV) * vec4(vColor, 1.0);
}
