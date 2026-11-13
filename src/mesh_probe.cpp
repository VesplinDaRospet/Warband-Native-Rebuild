// 诊断：dump 指定网格的 flags、UV 范围、顶点色统计
#include <cstdio>
#include "brf.h"
int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: mesh_probe <file.brf> <meshName>\n"); return 1; }
    try {
        brf::BrfFile bf = brf::parse(argv[1]);
        for (auto& m : bf.meshes) {
            if (m.name != argv[2]) continue;
            std::printf("mesh[%s] flags=%08X mat=%s pos=%zu verts=%zu tris=%zu extraFrames=%zu rigBones=%zu\n",
                        m.name.c_str(), m.flags, m.material.c_str(), m.positions.size(), m.verts.size(),
                        m.indices.size() / 3, m.extraFramePos.size(), m.rigging.size());
            float minu=9,maxu=-9,minv=9,maxv=-9;
            double r=0,g=0,b=0; size_t n=0;
            for (auto& v : m.verts) {
                minu=std::min(minu,v.uv.u); maxu=std::max(maxu,v.uv.u);
                minv=std::min(minv,v.uv.v); maxv=std::max(maxv,v.uv.v);
                r += (v.color & 0xFF); g += ((v.color >> 8) & 0xFF); b += ((v.color >> 16) & 0xFF);
                n++;
            }
            std::printf("  uv: u[%.3f,%.3f] v[%.3f,%.3f]\n", minu, maxu, minv, maxv);
            std::printf("  顶点色平均: R=%.0f G=%.0f B=%.0f\n", r/n, g/n, b/n);
            size_t white = 0;
            for (auto& v : m.verts) if ((v.color & 0xFFFFFF) == 0xFFFFFF) white++;
            std::printf("  纯白顶点: %zu/%zu\n", white, n);
            return 0;
        }
        std::printf("未找到网格 %s\n", argv[2]);
    } catch (const std::exception& e) { std::fprintf(stderr, "失败: %s\n", e.what()); return 1; }
    return 0;
}
