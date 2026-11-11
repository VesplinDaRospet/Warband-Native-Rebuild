#version 450
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec3 aNormal;
layout(location=3) in vec2 aUV;
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location=0) out vec3 vColor;
layout(location=1) out vec2 vUV;
void main(){
  gl_Position = pc.mvp * vec4(aPos, 1.0);
  vColor = aColor; vUV = aUV;
}
