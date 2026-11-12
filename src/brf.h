// brf.h —— 自研轻量 BRF 解析器（仅依赖 C++ 标准库）
// 格式事实来源：公开的 OpenBRF(GPLv2) 代码中体现的磁盘布局；本实现为独立编写。
// 布局要点（小端）：
//   字符串: u32 长度(<256) + 字节
//   Point3f 磁盘序: x, z, y
//   容器: 循环 [token字符串] ... 直到 "end"；可选 "rfver " + i32 版本
//   mesh: name, flags(u32), material, frame0.pos(vector<Point3f>),
//         rigging(vector<{i32 bone, vector<{i32 vidx, f32 weight}>}>),
//         nExtraFrames(i32), extra frames {i32 time, vector<Point3f> pos, vector<Point3f> norm},
//         verts(vector), faces(vector<3*i32>)
//   顶点: i32 index, u32 color(AABBGGRR), Point3f normal,
//         [flags bit16 置位: Point3f tangent + u8 tangi], Point2f uv
//   纹理: name, flags(u32)
//   材质: name, flags, shader, diffuseA, diffuseB, bump, enviro, [可选 spec 字符串], specular f32, r,g,b f32
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>

namespace brf {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec2 { float u = 0, v = 0; };

struct Vertex {
    int32_t index = 0;
    uint32_t color = 0xFFFFFFFF; // AABBGGRR
    Vec3 normal;
    Vec3 tangent;   // 仅版本1有
    uint8_t tangi = 0;
    bool hasTangent = false;
    Vec2 uv;
};

struct Mesh {
    std::string name, material;
    uint32_t flags = 0;
    std::vector<Vec3> positions;      // frame0 位置
    std::vector<std::vector<Vec3>> extraFramePos, extraFrameNorm;
    std::vector<int32_t> extraFrameTime;
    struct BoneWeight { int32_t bone; std::vector<std::pair<int32_t, float>> pairs; };
    std::vector<BoneWeight> rigging;  // 按骨骼组织的权重（原始磁盘布局）
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;    // 展平后的三角形索引
};

struct TextureEntry { std::string name; uint32_t flags = 0; };
struct ShaderEntry { std::string name, technique, fallback; uint32_t flags = 0, requirements = 0; };
struct MaterialEntry {
    std::string name, shader, diffuseA, diffuseB, bump, enviro, spec = "none";
    uint32_t flags = 0; float specular = 0, r = 1, g = 1, b = 1;
};

// ---------- 碰撞体（body；格式事实来自公开 OpenBRF 代码，独立实现） ----------
struct BodyPart {
    int type = 0;            // 0=sphere 1=capsule 2=manifold/face
    float radius = 0.f;
    Vec3 c, d;               // capsule：两端点；sphere：球心
    std::vector<Vec3> pts;   // manifold/face：顶点
    std::vector<std::vector<int32_t>> faces; // manifold：每个小面的顶点索引（face 类型为空=整块 pts 即一个面）
};
struct Body { std::string name; std::vector<BodyPart> parts; };

// ---------- 骨骼 / 动画（独立实现；格式事实来自公开 OpenBRF 文档与代码行为） ----------
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

struct Bone {
    std::string name;
    int attach = -1;   // 父骨索引，-1 = 根
    int b = 0;         // 磁盘上的第二 int（含义未用，保留）
    Vec3 col0, col1, col2; // 3x3 旋转，列主序（已做坐标系调整）
    Vec3 t;                // 平移（已做坐标系调整）
    std::vector<int> children;
};

struct Skeleton {
    std::string name;
    int root = -1;
    std::vector<Bone> bones;
};

struct AnimFrame {
    int index = 0;
    std::vector<Quat> rots; // 每骨一个，已做坐标系调整
    Vec3 tra;               // 根位移（磁盘原值）
};

struct SkeletonAnim {
    std::string name;
    std::vector<AnimFrame> frames; // 按出现过的 index 重建，缺样本沿用前一帧
};

struct BrfFile {
    int version = 0; // 0 = M&B 原生文件；1 = OpenBRF rfver1；2 = 顶点格式版本2
    std::vector<Mesh> meshes;
    std::vector<TextureEntry> textures;
    std::vector<ShaderEntry> shaders;
    std::vector<MaterialEntry> materials;
    std::vector<Skeleton> skeletons;
    std::vector<SkeletonAnim> anims;
    std::vector<Body> bodies;
    std::vector<std::string> unknownTokens;
};

class Reader {
public:
    explicit Reader(std::ifstream& f) : f_(f) {}
    uint32_t u32() { uint32_t v; read(&v, 4); return v; }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { uint32_t v = u32(); float r; std::memcpy(&r, &v, 4); return r; }
    uint8_t u8() { uint8_t v; read(&v, 1); return v; }
    std::string str() {
        uint32_t n = u32();
        if (n > 4096) throw std::runtime_error("BRF: 非法字符串长度 " + std::to_string(n));
        std::string s(n, '\0');
        if (n) read(s.data(), n);
        return s;
    }
    std::string strBody(uint32_t n) {
        std::string s(n, '\0');
        if (n) read(s.data(), n);
        return s;
    }
    void skip(size_t n) {
        for (size_t i=0;i<n;i++) { char c; read(&c,1); }
    }
    Vec3 point3() { // 磁盘序 x,z,y
        Vec3 p;
        p.x = f32(); p.z = f32(); p.y = f32();
        return p;
    }
    Vec2 point2() { Vec2 p; p.u = f32(); p.v = f32(); return p; }
    bool ok() const { return (bool)f_; }
private:
    void read(void* p, size_t n) { f_.read((char*)p, n); if (!f_) throw std::runtime_error("BRF: 文件提前结束"); }
    std::ifstream& f_;
};

inline Vec2 uvFlipped(Vec2 p) { p.v = 1.f - p.v; return p; }

// ---------- 碰撞体解析（body） ----------
static inline BodyPart loadBodyPart(Reader& r, const std::string& first) {
    BodyPart p;
    std::string t = first.empty() ? r.str() : first;
    if (t == "manifold" || t == "face") {
        p.type = 2;
        uint32_t n = r.u32();
        p.pts.resize(n);
        for (auto& v : p.pts) v = r.point3();
        if (t == "manifold") {
            int32_t k = r.i32();
            p.faces.resize((size_t)(k > 0 ? k : 0));
            for (int32_t i = 0; i < k; i++) {
                r.i32(); r.i32();            // ori, flags
                uint32_t c = r.u32();
                p.faces[(size_t)i].resize(c);
                for (uint32_t j = 0; j < c; j++) p.faces[(size_t)i][j] = r.i32();
            }
        } else {
            r.u32(); // flags
        }
    } else if (t == "capsule") {
        p.type = 1;
        p.radius = r.f32();
        p.c = r.point3();
        p.d = r.point3();
        r.u32(); // flags
    } else if (t == "sphere") {
        p.type = 0;
        p.radius = r.f32();
        p.c = r.point3();
        r.u32(); // flags
    } else throw std::runtime_error("BRF: 未知碰撞体类型 " + t);
    return p;
}
static inline void loadBodies(Reader& r, BrfFile& b) {
    uint32_t count = r.u32();
    b.bodies.resize(count);
    for (auto& body : b.bodies) {
        body.name = r.str();
        std::string first = r.str();
        if (first == "composite") {
            uint32_t n = r.u32();
            body.parts.resize(n);
            for (auto& p : body.parts) p = loadBodyPart(r, "");
        } else {
            body.parts.resize(1);
            body.parts[0] = loadBodyPart(r, first);
        }
    }
}
// ---------- 骨骼坐标系调整（与公开 OpenBRF 行为一致的独立实现） ----------
// adjust(v) = (v.z, -v.y, v.x)；用于非根骨与动画四元数
static inline Vec3 adjustSk(Vec3 v) { return Vec3{v.z, -v.y, v.x}; }
static inline Vec3 negV(Vec3 v) { return Vec3{-v.x, -v.y, -v.z}; }
// 动画四元数调整（vcg Point4f 分量序 w,x,y,z 下的 (p0,p3,-p2,p1)）：
// q' = (q.z, -q.y, q.x, q.w)，且两次复合 = 恒等（与 OpenBRF AdjustInv 一致）
static inline Quat adjustQuat(Quat q) { return Quat{q.z, -q.y, q.x, q.w}; }
// 根骨四元数再右乘 q0（vcg Quaternionf(0,s,0,s)：w=0,x=s,y=0,z=s）
// = 本结构 (x=s, y=0, z=s, w=0)，s=1/sqrt(2)，标准 Hamilton 乘法
static inline Quat quatMul(Quat a, Quat b) {
    return Quat{
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
    };
}
static inline Quat adjustRootQuat(Quat q) {
    constexpr float s = 0.7071067811865476f;
    return quatMul(q, Quat{s, 0.f, s, 0.f});
}

static inline Bone loadBone(Reader& r) {
    Bone bn;
    bn.attach = r.i32();
    uint32_t probe = r.u32();
    if (probe > 0 && probe < 99) { bn.name = r.strBody(probe); bn.b = r.i32(); }
    // 磁盘上是 4 组 vec3，依次名为 x,z,y,t；每组按 Point3f 磁盘序 x,z,y 读取
    // → 欧几里得 (X=f0, Y=f2, Z=f1)，与网格顶点同一套读取规则
    Vec3 px = r.point3();
    Vec3 pz = r.point3();
    Vec3 py = r.point3();
    Vec3 pt = r.point3();
    if (bn.attach >= 0) {
        bn.col0 = adjustSk(pz);
        bn.col1 = negV(adjustSk(py));
        bn.col2 = adjustSk(px);
        bn.t    = adjustSk(pt);
    } else {
        bn.col0 = pz;
        bn.col1 = negV(py);
        bn.col2 = px;
        bn.t    = pt;
    }
    return bn;
}

static inline Skeleton loadSkeletonOne(Reader& r) {
    Skeleton s;
    s.name = r.str();
    uint32_t n = r.u32();
    s.bones.resize(n);
    for (auto& b : s.bones) b = loadBone(r);
    for (size_t i = 0; i < s.bones.size(); ++i) {
        int a = s.bones[i].attach;
        if (a == -1) s.root = (int)i;
        else if (a >= 0 && (size_t)a < s.bones.size()) s.bones[a].children.push_back((int)i);
    }
    return s;
}

static inline void loadSkeletons(Reader& r, BrfFile& b) {
    uint32_t count = r.u32();
    b.skeletons.resize(count);
    for (auto& s : b.skeletons) s = loadSkeletonOne(r);
}

// 重建动画帧：每骨样本 (index, quat)，缺失 index 沿用前一帧；tra 同理
static inline void buildAnimFrames(
    const std::vector<std::vector<std::pair<int, Quat>>>& boneSamples,
    const std::vector<std::pair<int, Vec3>>& traSamples,
    SkeletonAnim& out) {
    std::vector<int> idxs;
    for (auto& bs : boneSamples)
        for (auto& kv : bs) idxs.push_back(kv.first);
    for (auto& kv : traSamples) idxs.push_back(kv.first);
    std::sort(idxs.begin(), idxs.end());
    idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
    size_t nBones = boneSamples.size();
    std::vector<size_t> bonePos(nBones, 0);
    size_t traPos = 0;
    out.frames.resize(idxs.size());
    for (size_t f = 0; f < idxs.size(); ++f) {
        int idx = idxs[f];
        AnimFrame& fr = out.frames[f];
        fr.index = idx;
        fr.rots.resize(nBones);
        if (f > 0) fr.rots = out.frames[f - 1].rots; // 隐式帧沿用前帧
        for (size_t b = 0; b < nBones; ++b) {
            auto& bs = boneSamples[b];
            while (bonePos[b] < bs.size() && bs[bonePos[b]].first < idx) ++bonePos[b];
            // 取最后一个 <= idx 的样本；多个同 index 取最后一个
            size_t p = bonePos[b];
            while (p + 1 < bs.size() && bs[p + 1].first <= idx) ++p;
            if (p < bs.size() && bs[p].first <= idx) fr.rots[b] = bs[p].second;
            else if (!bs.empty()) fr.rots[b] = bs[0].second; // 首样本之前兜底
        }
        if (f > 0) fr.tra = out.frames[f - 1].tra;
        while (traPos < traSamples.size() && traSamples[traPos].first < idx) ++traPos;
        size_t tp = traPos;
        while (tp + 1 < traSamples.size() && traSamples[tp + 1].first <= idx) ++tp;
        if (tp < traSamples.size() && traSamples[tp].first <= idx) fr.tra = traSamples[tp].second;
        else if (!traSamples.empty()) fr.tra = traSamples[0].second;
    }
}

static inline SkeletonAnim loadSkeletonAnimOne(Reader& r) {
    SkeletonAnim a;
    a.name = r.str();
    uint32_t nBones = r.u32();
    std::vector<std::vector<std::pair<int, Quat>>> boneSamples(nBones);
    for (uint32_t i = 0; i < nBones; ++i) {
        uint32_t c = r.u32();
        boneSamples[i].resize(c);
        for (uint32_t j = 0; j < c; ++j) {
            int idx = r.i32();
            // Point4f 磁盘序 Y,W,Z,X → 四元数 (x=第1值, y=第3值, z=第2值, w=第4值)
            float xv = r.f32(), zv = r.f32(), yv = r.f32(), wv = r.f32();
            Quat q{xv, yv, zv, wv};
            q = adjustQuat(q);
            if (i == 0) q = adjustRootQuat(q);
            boneSamples[i][j] = {idx, q};
        }
    }
    uint32_t nTrans = r.u32();
    std::vector<std::pair<int, Vec3>> traSamples(nTrans);
    for (uint32_t j = 0; j < nTrans; ++j) {
        int idx = r.i32();
        traSamples[j] = {idx, r.point3()}; // Point3f 磁盘序 x,z,y → (X,Y,Z)，不做坐标系调整
    }
    buildAnimFrames(boneSamples, traSamples, a);
    return a;
}

static inline void loadSkeletonAnims(Reader& r, BrfFile& b) {
    uint32_t count = r.u32();
    b.anims.resize(count);
    for (auto& a : b.anims) a = loadSkeletonAnimOne(r);
}

// 解析一个对象向量（元素自带 Load 语义）
template <class T, class LoadFn>
inline void loadVector(Reader& r, std::vector<T>& out, LoadFn load) {
    uint32_t n = r.u32();
    out.resize(n);
    for (auto& e : out) load(r, e);
}

inline BrfFile parse(const std::string& path, bool verbose = false) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("打不开文件: " + path);
    Reader r(f);
    BrfFile b;
    int vertFormat = 2; // 默认 warband v2；文件整体 version==1 时为 OpenBRF 文件格式
    while (true) {
        std::string token = r.str();
        if (verbose) std::printf("token: %s\n", token.c_str());
        if (token == "end") break;
        if (token == "rfver ") { b.version = r.i32(); continue; }
        if (token == "texture") {
            loadVector(r, b.textures, [](Reader& rr, TextureEntry& t) {
                t.name = rr.str(); t.flags = rr.u32();
            });
        } else if (token == "shader") {
            loadVector(r, b.shaders, [](Reader& rr, ShaderEntry& s) {
                s.name = rr.str(); s.flags = rr.u32(); s.requirements = rr.u32();
                s.technique = rr.str();
                uint32_t k = rr.u32();
                if (k) s.fallback = rr.str();
                uint32_t nOpt = rr.u32();
                for (uint32_t i = 0; i < nOpt; ++i) { rr.i32(); rr.u32(); rr.u32(); rr.u32(); }
            });
        } else if (token == "material") {
            loadVector(r, b.materials, [](Reader& rr, MaterialEntry& m) {
                m.name = rr.str(); m.flags = rr.u32();
                m.shader = rr.str(); m.diffuseA = rr.str(); m.diffuseB = rr.str();
                m.bump = rr.str(); m.enviro = rr.str();
                // spec 是可选字符串：先探测长度字段；0<len<99 视为字符串，
                // 否则这 4 字节其实是 specular float 的位模式。
                uint32_t probe = rr.u32();
                if (probe > 0 && probe < 99) {
                    m.spec = rr.strBody(probe);
                    m.specular = rr.f32();
                } else {
                    m.spec = "none";
                    std::memcpy(&m.specular, &probe, 4);
                }
                m.r = rr.f32(); m.g = rr.f32(); m.b = rr.f32();
            });
        } else if (token == "mesh") {
            uint32_t n = r.u32();
            b.meshes.resize(n);
            for (auto& m : b.meshes) {
                m.name = r.str(); m.flags = r.u32(); m.material = r.str();
                if (verbose) std::printf("  mesh[%s] flags=%08X mat=%s\n", m.name.c_str(), m.flags, m.material.c_str());
                vertFormat = (m.flags & (1u << 16)) ? 1 : 2; // warband 顶点版本
                loadVector(r, m.positions, [](Reader& rr, Vec3& p) { p = rr.point3(); });
                // rigging: vector<{i32 bone, vector<{i32 vidx, f32 weight}>}>
                uint32_t nRig = r.u32();
                m.rigging.resize(nRig);
                for (auto& rig : m.rigging) {
                    rig.bone = r.i32();
                    uint32_t np = r.u32();
                    rig.pairs.resize(np);
                    for (auto& p : rig.pairs) { p.first = r.i32(); p.second = r.f32(); }
                }
                int32_t nFrames = r.i32();
                m.extraFrameTime.resize(nFrames);
                m.extraFramePos.resize(nFrames);
                m.extraFrameNorm.resize(nFrames);
                for (int32_t i = 0; i < nFrames; ++i) {
                    m.extraFrameTime[i] = r.i32();
                    loadVector(r, m.extraFramePos[i], [](Reader& rr, Vec3& p) { p = rr.point3(); });
                    loadVector(r, m.extraFrameNorm[i], [](Reader& rr, Vec3& p) { p = rr.point3(); });
                }
                loadVector(r, m.verts, [&](Reader& rr, Vertex& v) {
                    v.index = rr.i32(); v.color = rr.u32();
                    v.normal = rr.point3();
                    if (vertFormat == 1) { v.tangent = rr.point3(); v.tangi = rr.u8(); v.hasTangent = true; }
                    v.uv = uvFlipped(rr.point2());
                });
                uint32_t nFace = r.u32();
                m.indices.resize((size_t)nFace * 3);
                for (uint32_t i = 0; i < nFace * 3; ++i) m.indices[i] = (uint32_t)r.i32();
            }
        } else if (token == "skeleton") {
            loadSkeletons(r, b);
        } else if (token == "skeleton_anim") {
            loadSkeletonAnims(r, b);
        } else if (token == "body") {
            loadBodies(r, b);
        } else {
            b.unknownTokens.push_back(token);
            if (verbose) std::printf("  [未知 token，停止]\n");
            break;
        }
    }
    return b;
}

} // namespace brf
