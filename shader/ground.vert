#version 450
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aWeight; // 三图层权重，已归一化，和为 1
layout(location=2) in vec3 aTint;   // leveling 顶点色 0..1
layout(location=3) in vec3 aNormal;
layout(location=4) in vec2 aUV0;
layout(location=5) in vec2 aUV1;
layout(location=6) in vec2 aUV2;
layout(push_constant) uniform PC {
  mat4 mvp;
  vec4 eye;   // 相机世界位置 xyz（渲染坐标）
  vec4 fog;   // x=fogStart y=fogEnd
} pc;
layout(location=0) out vec3 vWeight;
layout(location=1) out vec3 vTint;
layout(location=2) out vec3 vNormal;
layout(location=3) out vec2 vUV0;
layout(location=4) out vec2 vUV1;
layout(location=5) out vec2 vUV2;
layout(location=6) out vec3 vWorld;
void main(){
  gl_Position = pc.mvp * vec4(aPos, 1.0);
  vWorld = aPos;
  vWeight = aWeight;
  vTint = aTint;
  vNormal = aNormal;
  vUV0 = aUV0; vUV1 = aUV1; vUV2 = aUV2;
}
