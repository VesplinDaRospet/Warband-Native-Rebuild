// map.h —— 解析 Mount&Blade Warband Modules/Native/map.txt（格式依据公开 modding wiki，独立实现）
// 结构：顶点数 V / V 行 x y z（x,y=水平，z=高度）/ 面数 F / F 行 "terrain 0 3 i0 i1 i2"
#pragma once
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct MapMesh {
    std::vector<float> verts;   // 每顶点 11 float：pos(3) color(3) normal(3) uv(2)
    std::vector<uint32_t> idx;
    int terrain = -1;
};

struct MapData {
    std::vector<std::array<float,3>> pos;
    std::vector<std::array<uint32_t,3>> tris;   // 全部三角形（顶点索引）
    std::vector<int> faceTerrain;               // 每个三角形的地形码
    std::vector<MapMesh> meshes; // 下标 = terrain code 0..13（没面的为空）
    float minX=0, maxX=0, minY=0, maxY=0;
};

static inline std::pair<std::string, std::array<float,3>> mapTexture(int t) {
    switch (t) {
        case 0:  return {"map_misc",     {0.22f,0.38f,0.62f}}; // 水
        case 1:  return {"mountain",     {1,1,1}};             // 山
        case 2:  return {"map_steppe",   {1,1,1}};             // 草原
        case 3:  return {"plain",        {1,1,1}};             // 平原
        case 4:  return {"snow",         {1,1,1}};             // 雪
        case 5:  return {"desert",       {1,1,1}};             // 沙漠
        case 7:  return {"map_misc",     {0.55f,0.45f,0.35f}}; // 桥
        case 8:  return {"river",        {0.55f,0.75f,1.0f}};  // 河
        case 10: return {"map_steppe",   {0.55f,0.75f,0.50f}}; // 草原林
        case 11: return {"map_steppe",   {0.45f,0.68f,0.42f}}; // 森林
        case 12: return {"snow",         {0.80f,0.90f,0.80f}}; // 雪林
        case 13: return {"desert",       {0.70f,0.60f,0.40f}}; // 沙漠林
        default: return {"map_steppe",   {1,1,1}};
    }
}

static inline MapData loadMap(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("map.txt 打不开: " + path);
    MapData d;
    int V = 0; f >> V;
    if (V <= 0 || V > 1000000) throw std::runtime_error("map.txt 顶点数异常");
    d.pos.resize((size_t)V);
    d.minX = d.minY = 1e30f; d.maxX = d.maxY = -1e30f;
    for (int i = 0; i < V; i++) {
        float x, y, z; f >> x >> y >> z;
        if (!f) throw std::runtime_error("map.txt 顶点提前结束");
        d.pos[(size_t)i] = {x, y, z};
        d.minX = std::min(d.minX, x); d.maxX = std::max(d.maxX, x);
        d.minY = std::min(d.minY, y); d.maxY = std::max(d.maxY, y);
    }
    int F = 0; f >> F;
    if (F <= 0 || F > 1000000) throw std::runtime_error("map.txt 面数异常");
    d.meshes.resize(14);
    for (int i = 0; i < 14; i++) d.meshes[i].terrain = i;
    auto tint = [](int t) { return mapTexture(t).second; };
    for (int i = 0; i < F; i++) {
        int t = 0, u0 = 0, u1 = 0, a = 0, b = 0, c = 0;
        f >> t >> u0 >> u1 >> a >> b >> c;
        if (!f) throw std::runtime_error("map.txt 面提前结束");
        if (t < 0 || t >= 14 || a < 0 || b < 0 || c < 0 || a >= V || b >= V || c >= V) continue;
        d.tris.push_back({(uint32_t)a, (uint32_t)b, (uint32_t)c});
        d.faceTerrain.push_back(t);
        auto& m = d.meshes[(size_t)t];
        uint32_t base = (uint32_t)(m.verts.size() / 11);
        const float tile = 8.0f;
        const auto& p0 = d.pos[(size_t)a], p1 = d.pos[(size_t)b], p2 = d.pos[(size_t)c];
        float e1x = p1[0]-p0[0], e1y = p1[1]-p0[1], e1z = p1[2]-p0[2];
        float e2x = p2[0]-p0[0], e2y = p2[1]-p0[1], e2z = p2[2]-p0[2];
        float nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
        float nl = sqrtf(nx*nx + ny*ny + nz*nz); if (nl < 1e-6f) nl = 1.f;
        nx /= nl; ny /= nl; nz /= nl;
        auto col = tint(t);
        const std::array<float,3>* ps[3] = {&p0, &p1, &p2};
        for (int k = 0; k < 3; k++) {
            const auto& p = *ps[k];
            m.verts.push_back(p[0]); m.verts.push_back(p[1]); m.verts.push_back(p[2]);
            m.verts.push_back(col[0]); m.verts.push_back(col[1]); m.verts.push_back(col[2]);
            m.verts.push_back(nx); m.verts.push_back(ny); m.verts.push_back(nz);
            m.verts.push_back(p[0]/tile); m.verts.push_back(p[1]/tile);
        }
        m.idx.push_back(base); m.idx.push_back(base+1); m.idx.push_back(base+2);
    }
    return d;
}
