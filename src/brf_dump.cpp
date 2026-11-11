// brf_dump.cpp —— 解析 BRF 并打印概要（最小闭环第一步的验证工具）
#include "brf.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: brf_dump <file.brf>\n"); return 1; }
    try {
        brf::BrfFile b = brf::parse(argv[1], false);
        std::printf("version=%d meshes=%zu textures=%zu materials=%zu unknown=%zu\n",
                    b.version, b.meshes.size(), b.textures.size(), b.materials.size(), b.unknownTokens.size());
        for (const auto& t : b.unknownTokens) std::printf("  unknown token: %s\n", t.c_str());
        size_t shown = 0;
        for (const auto& m : b.meshes) {
            if (shown++ < 12)
                std::printf("  mesh[%s] flags=%08X mat=%s pos=%zu verts=%zu tris=%zu extraFrames=%zu rigBones=%zu\n",
                            m.name.c_str(), m.flags, m.material.c_str(), m.positions.size(),
                            m.verts.size(), m.indices.size() / 3, m.extraFramePos.size(), m.rigging.size());
        }
        for (const auto& t : b.textures)
            std::printf("  tex[%s] flags=%08X\n", t.name.c_str(), t.flags);
        for (const auto& mt : b.materials)
            std::printf("  mat[%s] shader=%s diffA=%s diffB=%s bump=%s enviro=%s spec=%s\n",
                        mt.name.c_str(), mt.shader.c_str(), mt.diffuseA.c_str(), mt.diffuseB.c_str(),
                        mt.bump.c_str(), mt.enviro.c_str(), mt.spec.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::printf("ERROR: %s\n", e.what());
        return 1;
    }
}
