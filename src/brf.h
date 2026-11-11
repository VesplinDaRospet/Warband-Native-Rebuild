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

struct BrfFile {
    int version = 0; // 0 = M&B 原生文件；1 = OpenBRF rfver1；2 = 顶点格式版本2
    std::vector<Mesh> meshes;
    std::vector<TextureEntry> textures;
    std::vector<ShaderEntry> shaders;
    std::vector<MaterialEntry> materials;
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

// ---------- 跳过不需要的对象类型（skeleton / skeleton_anim / body）----------
static inline void skipBodyPart(Reader& r, const std::string& first) {
    std::string t = first.empty() ? r.str() : first;
    if (t == "manifold") {
        uint32_t n = r.u32(); r.skip((size_t)n*12);
        int32_t k = r.i32();
        for (int32_t i=0;i<k;i++) { r.skip(8); uint32_t c = r.u32(); r.skip((size_t)c*4); }
    } else if (t == "capsule") { r.skip(32); }
    else if (t == "sphere") { r.skip(20); }
    else if (t == "face") { uint32_t n = r.u32(); r.skip((size_t)n*12); r.skip(4); }
    else throw std::runtime_error("BRF: 未知碰撞体类型 " + t);
}
static inline void skipBody(Reader& r) {
    uint32_t count = r.u32();
    for (uint32_t i=0;i<count;i++) {
        r.str(); // name
        std::string first = r.str();
        if (first == "composite") {
            uint32_t n = r.u32();
            for (uint32_t j=0;j<n;j++) skipBodyPart(r, "");
        } else {
            skipBodyPart(r, first);
        }
    }
}
static inline void skipSkeleton(Reader& r) {
    uint32_t count = r.u32();
    for (uint32_t i=0;i<count;i++) {
        r.str(); // name
        uint32_t n = r.u32();
        for (uint32_t j=0;j<n;j++) {
            r.i32(); // attach
            uint32_t probe = r.u32();
            if (probe > 0 && probe < 99) { r.skip(probe); r.i32(); }
            r.skip(48); // 4 个 Point3f
        }
    }
}
static inline void skipAnimation(Reader& r) {
    uint32_t count = r.u32();
    for (uint32_t i=0;i<count;i++) {
        r.str(); // name
        uint32_t nBones = r.u32();
        for (uint32_t j=0;j<nBones;j++) {
            uint32_t c = r.u32(); r.skip((size_t)c*20); // TmpCas4 = i32 + Point4f
        }
        uint32_t nTrans = r.u32();
        r.skip((size_t)nTrans*16); // TmpCas3 = i32 + Point3f
    }
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
            skipSkeleton(r);
        } else if (token == "skeleton_anim") {
            skipAnimation(r);
        } else if (token == "body") {
            skipBody(r);
        } else {
            b.unknownTokens.push_back(token);
            if (verbose) std::printf("  [未知 token，停止]\n");
            break;
        }
    }
    return b;
}

} // namespace brf
