#version 450
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec3 aNormal;
layout(location=3) in vec2 aUV;
layout(push_constant) uniform PC {
  mat4 mvp;
  vec4 eye;   // 相机世界位置 xyz（渲染坐标）
  vec4 fog;   // x=fogStart y=fogEnd
} pc;
layout(location=0) out vec3 vColor;
layout(location=1) out vec3 vNormal;
layout(location=2) out vec2 vUV;
layout(location=3) out vec3 vWorld;
void main(){
  gl_Position = pc.mvp * vec4(aPos, 1.0);
  vWorld = aPos;
  vColor = aColor; vNormal = aNormal; vUV = aUV;
}
