// obj_export.cpp —— 从 BRF 导出指定网格为 OBJ（验证解析正确性的中间件）
#include "brf.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: obj_export <file.brf> <meshName> [out.obj]\n"); return 1; }
    try {
        brf::BrfFile b = brf::parse(argv[1]);
        const brf::Mesh* m = nullptr;
        for (auto& x : b.meshes) if (x.name == argv[2]) { m = &x; break; }
        if (!m) { std::printf("FAIL mesh not found: %s\n", argv[2]); return 1; }
        std::string out = argc >= 4 ? argv[3] : (m->name + ".obj");
        FILE* f = fopen(out.c_str(), "w");
        if (!f) { std::printf("FAIL open %s\n", out.c_str()); return 1; }
        // 建立 vert -> position 的映射（重复 index 直接铺开）
        std::vector<int> posOf(m->verts.size());
        for (size_t i = 0; i < m->verts.size(); ++i) {
            int pidx = m->verts[i].index;
            posOf[i] = (pidx >= 0 && pidx < (int)m->positions.size()) ? pidx : 0;
        }
        for (size_t i = 0; i < m->verts.size(); ++i) {
            auto& p = m->positions[posOf[i]];
            fprintf(f, "v %.6f %.6f %.6f\n", p.x, p.y, p.z);
        }
        for (size_t i = 0; i + 2 < m->indices.size(); i += 3) {
            fprintf(f, "f %u %u %u\n", m->indices[i] + 1, m->indices[i+1] + 1, m->indices[i+2] + 1);
        }
        fclose(f);
        std::printf("OK %s (verts=%zu tris=%zu)\n", out.c_str(), m->verts.size(), m->indices.size()/3);
        return 0;
    } catch (const std::exception& e) {
        std::printf("ERROR: %s\n", e.what());
        return 1;
    }
}
