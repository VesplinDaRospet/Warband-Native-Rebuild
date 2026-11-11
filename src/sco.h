// sco.h —— 自研 SceneObj(.sco) 解析器（只读场景物件段）
// 格式依据：公开的 Swyter mab-tools 010 模板与 TaleWorlds 论坛 cmpxchg8b 文档。
// 小端；头 = int32 magic(-717, 0xFFFFFD33) + int32 version(4)；随后 mission_objects。
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace sco {

struct Prop {
    uint32_t type = 0;      // 0=Scene Prop 1=Entry 2=Item 4=Plant 5=Passage
    uint32_t id = 0;
    uint32_t unk = 0;
    float m[9] = {};        // 3x3 row-major 仿射矩阵（旋转/缩放）
    float pos[3] = {};      // x y z
    std::string tag;
    uint32_t entryNo = 0, menuNo = 0;
    float scale[3] = {1,1,1};
};

struct Scene {
    int version = 0;
    std::vector<Prop> objects;
};

class R {
public:
    R(std::ifstream& f) : f_(f) {}
    uint32_t u32() { uint32_t v; rd(&v,4); return v; }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { uint32_t v=u32(); float r; memcpy(&r,&v,4); return r; }
    std::string tag() { uint32_t n=u32(); if(n>4096) throw std::runtime_error("SCO tag 异常"); std::string s(n,'\0'); if(n) rd(s.data(),n); return s; }
    void skip(size_t n) { for (size_t i=0;i<n;i++) { char c; rd(&c,1); } }
private:
    void rd(void* p, size_t n) { f_.read((char*)p,n); if(!f_) throw std::runtime_error("SCO 提前结束"); }
    std::ifstream& f_;
};

inline Scene load(const std::string& path, bool verbose=false) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("SCO 打不开: " + path);
    R r(f);
    Scene s;
    int magic = r.i32();
    if (magic != -717) throw std::runtime_error("SCO 魔数不对: " + std::to_string(magic));
    s.version = r.i32();
    if (verbose) std::printf("magic OK version=%d\n", s.version);
    uint32_t n = r.u32();
    s.objects.resize(n);
    for (auto& o : s.objects) {
        o.type = r.u32(); o.id = r.u32(); o.unk = r.u32();
        for (int i=0;i<9;i++) o.m[i]=r.f32();
        // 磁盘序 x, z, y（M&B 惯例），转为逻辑 x, y(高), z
        o.pos[0] = r.f32(); float pz = r.f32(); float py = r.f32();
        o.pos[1] = py; o.pos[2] = pz;
        o.tag = r.tag();
        o.entryNo = r.u32(); o.menuNo = r.u32();
        if (s.version > 2) for (int i=0;i<3;i++) o.scale[i]=r.f32();
    }
    return s;
}

struct TerrainLayer {
    int32_t index = 0;              // 0..10 = ground_specs 层；-7793 高程；-12565 顶点色
    std::string name;
    uint32_t enabled = 0;
    std::vector<uint8_t> cells;     // width*height，逐行；ground 层为 ubyte 强度
    std::vector<uint32_t> colors;   // leveling：逐 cell r,g,b,a（小端 u32）
};

struct Terrain {
    uint32_t width = 0, height = 0;
    std::vector<float> elevation; // width*height，逐行
    std::vector<TerrainLayer> layers;
    const TerrainLayer* findLayer(int32_t idx) const {
        for (auto& l : layers) if (l.index == idx) return &l;
        return nullptr;
    }
};

// 在 mission_objects 之后继续解析 ai_mesh + terrain（version>3 才有）
inline bool loadTerrain(const std::string& path, Terrain& t, bool verbose=false) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("SCO 打不开: " + path);
    R r(f);
    int magic = r.i32();
    if (magic != -717) throw std::runtime_error("SCO 魔数不对");
    int version = r.i32();
    uint32_t n = r.u32();
    for (uint32_t i=0;i<n;i++) {
        r.u32(); r.u32(); r.u32();          // type, id, unk
        r.skip(9*4);                         // matrix
        r.skip(3*4);                         // pos
        uint32_t tl = r.u32(); r.skip(tl);   // tag
        r.u32(); r.u32();                    // entry/menu
        if (version > 2) r.skip(3*4);        // scale
    }
    if (version <= 3) return false;
    // ai_mesh：逐结构读（格式依据 010 模板）
    uint32_t sectionSize = r.u32();
    uint32_t vCount = r.u32();
    r.skip((size_t)vCount*12);
    uint32_t eCount = r.u32();
    r.skip((size_t)eCount*20); // 每条边：5 个 int32
    uint32_t fCount = r.u32();
    for (uint32_t i=0;i<fCount;i++) {
        uint32_t c = r.u32();
        r.skip((size_t)c*8);
        uint32_t more = r.u32();
        if (more) r.u32();
    }
    // terrain
    int32_t tmagic = r.i32();
    if (verbose) std::printf("terrain magic=0x%08X\n", (unsigned)tmagic);
    uint32_t secSize = r.u32();
    uint32_t nLayers = r.u32();
    t.width = r.u32(); t.height = r.u32();
    uint32_t blockCount = t.width * t.height;
    for (uint32_t li=0; li<nLayers; li++) {
        int32_t index = r.i32();
        std::string name = r.tag();
        uint32_t enabled = r.u32();
        TerrainLayer layer;
        layer.index = index; layer.name = name; layer.enabled = enabled;
        if (enabled) layer.cells.resize(blockCount, 0);
        if (index == -12565 && enabled) layer.colors.resize(blockCount, 0);
        uint32_t remaining = blockCount;
        size_t fillPos = 0;
        if (enabled && remaining > 0) {
            while (remaining > 0) {
                int32_t rle = r.i32();
                fillPos += rle;
                remaining -= rle;
                if (remaining <= 0) break;
                uint32_t cnt = r.u32();
                if (index == -7793) { // ground_elevation：浮点高度（RLE 跳过块不写）
                    if (t.elevation.size() < fillPos + cnt) t.elevation.resize(fillPos + cnt);
                    for (uint32_t j=0;j<cnt;j++) t.elevation[fillPos+j] = r.f32();
                } else if (index == -12565) { // ground_leveling：逐 cell 颜色
                    for (uint32_t j=0;j<cnt;j++) { uint32_t v = r.u32(); layer.colors[fillPos+j] = v; }
                } else { // ground 层：逐 cell ubyte 强度
                    for (uint32_t j=0;j<cnt;j++) { uint8_t v; f.read((char*)&v,1); if(!f) throw std::runtime_error("SCO 提前结束"); layer.cells[fillPos+j] = v; }
                }
                fillPos += cnt;
                remaining -= cnt;
            }
        }
        t.layers.push_back(std::move(layer));
        if (verbose) std::printf("layer[%d] %s enabled=%u\n", index, name.c_str(), enabled);
    }
    return !t.elevation.empty();
}

} // namespace sco
