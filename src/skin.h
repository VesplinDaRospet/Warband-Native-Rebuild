// skin.h —— 自研 CPU 骨骼蒙皮（列主序 4x4；格式事实来自公开资料，独立实现）
// 用法：skin::bindGlobals(skel, bind) → skin::animGlobals(skel, frame, anim)
//       → skinMatrices(bind, anim, sk) → skinMesh(mesh, sk, outPos, outNorm)
// 输出坐标为 OpenBRF 欧几里得坐标；渲染坐标转换用 toRender()。
#pragma once
#include "brf.h"
#include <vector>

namespace skin {

struct M44 { float m[16]; };

inline M44 identity() {
    M44 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

inline M44 translate(float x, float y, float z) {
    M44 r = identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

inline M44 mul(const M44& a, const M44& b) {
    M44 r{};
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += a.m[k*4+row] * b.m[c*4+k];
            r.m[c*4+row] = s;
        }
    return r;
}

inline M44 invertAffine(const M44& a) {
    float r[9];
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) r[c*3+row] = a.m[c*4+row];
    float rt[9];
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) rt[c*3+row] = r[row*3+c];
    M44 o{};
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) o.m[c*4+row] = rt[c*3+row];
    for (int row = 0; row < 3; row++) {
        float s = 0.f;
        for (int k = 0; k < 3; k++) s += rt[k*3+row] * a.m[12+k];
        o.m[12+row] = -s;
    }
    o.m[15] = 1.f;
    return o;
}

inline void xformPos(const M44& m, brf::Vec3& p) {
    float x = p.x, y = p.y, z = p.z;
    p.x = m.m[0]*x + m.m[4]*y + m.m[8]*z + m.m[12];
    p.y = m.m[1]*x + m.m[5]*y + m.m[9]*z + m.m[13];
    p.z = m.m[2]*x + m.m[6]*y + m.m[10]*z + m.m[14];
}

inline void xformDir(const M44& m, brf::Vec3& n) {
    float x = n.x, y = n.y, z = n.z;
    n.x = m.m[0]*x + m.m[4]*y + m.m[8]*z;
    n.y = m.m[1]*x + m.m[5]*y + m.m[9]*z;
    n.z = m.m[2]*x + m.m[6]*y + m.m[10]*z;
}

// 骨骼 bind 旋转：列主序，列 = (col0,col1,col2)（与 ref 四元数调整组合后自洽；
// 探针验证：该组合 frame0 skin trace≈2.42、其余约定均 ≪3）
inline M44 boneBindT(const brf::Bone& b) {
    M44 r{};
    r.m[0] = b.col0.x; r.m[1] = b.col0.y; r.m[2] = b.col0.z;
    r.m[4] = b.col1.x; r.m[5] = b.col1.y; r.m[6] = b.col1.z;
    r.m[8] = b.col2.x; r.m[9] = b.col2.y; r.m[10] = b.col2.z;
    r.m[15] = 1.f;
    return r;
}

// 动画四元数(x,y,z,w) → 3x3 旋转，列主序存 R^T（与 bind 的 R^T 约定一致）
inline M44 quatT(brf::Quat q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float r[9] = { // 标准旋转矩阵，行主序
        1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w),
        2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w),
        2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y),
    };
    M44 m{};
    // 列主序存 R^T：col k = R 的第 k 行（与 boneBindT 约定一致）
    m.m[0] = r[0]; m.m[1] = r[1]; m.m[2] = r[2];
    m.m[4] = r[3]; m.m[5] = r[4]; m.m[6] = r[5];
    m.m[8] = r[6]; m.m[9] = r[7]; m.m[10] = r[8];
    m.m[15] = 1.f;
    return m;
}

// bind 全局（列主序：col(A*B)=col(B)*col(A)，故 local × parent）
inline void bindGlobals(const brf::Skeleton& s, std::vector<M44>& out) {
    out.assign(s.bones.size(), identity());
    if (s.root < 0) return;
    std::vector<int> stack{s.root};
    while (!stack.empty()) {
        int i = stack.back(); stack.pop_back();
        const brf::Bone& b = s.bones[i];
        M44 par = (b.attach >= 0) ? out[b.attach] : identity();
        out[i] = mul(mul(translate(b.t.x, b.t.y, b.t.z), boneBindT(b)), par);
        for (int c : b.children) stack.push_back(c);
    }
}

// 动画全局（列主序：local × parent）
inline void animGlobals(const brf::Skeleton& s, const brf::AnimFrame& fr, std::vector<M44>& out) {
    out.assign(s.bones.size(), identity());
    if (s.root < 0) return;
    std::vector<int> stack{s.root};
    while (!stack.empty()) {
        int i = stack.back(); stack.pop_back();
        const brf::Bone& b = s.bones[i];
        M44 par = (b.attach >= 0) ? out[b.attach] : identity();
        M44 rootT = (i == s.root) ? translate(fr.tra.x, fr.tra.y, fr.tra.z) : identity();
        if ((size_t)i >= fr.rots.size()) { out[i] = par; continue; }
        out[i] = mul(mul(mul(rootT, translate(b.t.x, b.t.y, b.t.z)), quatT(fr.rots[i])), par);
        for (int c : b.children) stack.push_back(c);
    }
}

// 蒙皮矩阵 = 动画全局 × bind全局^-1（OpenBRF：res[i] = anim[i] * Inverse(bind[i])）
inline void skinMatrices(const std::vector<M44>& bind, const std::vector<M44>& anim,
                         std::vector<M44>& out) {
    out.resize(anim.size());
    for (size_t i = 0; i < anim.size(); i++) {
        M44 ib = (i < bind.size()) ? invertAffine(bind[i]) : identity();
        out[i] = mul(anim[i], ib);
    }
}

// rigging 磁盘布局（按骨组织）→ 按顶点组织的权重
inline void buildWeights(const brf::Mesh& m,
                         std::vector<std::vector<std::pair<int, float>>>& perVertex,
                         size_t vertexCount) {
    perVertex.assign(vertexCount, {});
    for (auto& rig : m.rigging)
        for (auto& p : rig.pairs)
            if (p.first >= 0 && (size_t)p.first < vertexCount)
                perVertex[p.first].push_back({rig.bone, p.second});
}

// CPU 蒙皮：输入 BRF 原始顶点，输出 OpenBRF 坐标；无权重顶点原样保留
inline void skinMesh(const brf::Mesh& m, const std::vector<M44>& sk,
                     std::vector<brf::Vec3>& outPos, std::vector<brf::Vec3>& outNorm) {
    std::vector<std::vector<std::pair<int, float>>> vw;
    buildWeights(m, vw, m.positions.size());
    outPos.resize(m.positions.size());
    outNorm.resize(m.verts.size());
    for (size_t v = 0; v < m.positions.size(); v++) {
        brf::Vec3 p = m.positions[v], acc{0, 0, 0};
        float wsum = 0.f;
        if (vw[v].empty()) { acc = p; }
        else {
            for (auto& bw : vw[v]) {
                if (bw.first < 0 || (size_t)bw.first >= sk.size()) continue;
                brf::Vec3 q = p;
                xformPos(sk[bw.first], q);
                acc.x += q.x * bw.second; acc.y += q.y * bw.second; acc.z += q.z * bw.second;
                wsum += bw.second;
            }
            if (wsum > 1e-6f) { acc.x /= wsum; acc.y /= wsum; acc.z /= wsum; }
            else acc = p;
        }
        outPos[v] = acc;
    }
    // 法线按顶点条目（verts 可能引用共享 index），先算每位置的加权法线
    std::vector<brf::Vec3> posN(m.positions.size(), brf::Vec3{0, 0, 0});
    for (auto& v : m.verts) {
        if (v.index < 0 || (size_t)v.index >= m.positions.size()) continue;
        brf::Vec3 n = v.normal, acc{0, 0, 0};
        float wsum = 0.f;
        if (!vw[v.index].empty()) {
            for (auto& bw : vw[v.index]) {
                if (bw.first < 0 || (size_t)bw.first >= sk.size()) continue;
                brf::Vec3 q = n;
                xformDir(sk[bw.first], q);
                acc.x += q.x * bw.second; acc.y += q.y * bw.second; acc.z += q.z * bw.second;
                wsum += bw.second;
            }
            if (wsum > 1e-6f) { acc.x /= wsum; acc.y /= wsum; acc.z /= wsum; }
            else acc = n;
        } else acc = n;
        posN[v.index] = acc;
    }
    for (size_t i = 0; i < m.verts.size(); i++) {
        int idx = m.verts[i].index;
        outNorm[i] = (idx >= 0 && (size_t)idx < posN.size()) ? posN[idx] : brf::Vec3{0, 0, 1};
    }
}

// OpenBRF 坐标 → 渲染坐标（x'=ob.z 左右, y'=ob.x 南北, z'=ob.y 高度）
inline brf::Vec3 toRender(brf::Vec3 ob) { return brf::Vec3{ob.z, ob.x, ob.y}; }

} // namespace skin
