// 快速诊断：dump skel_human 骨名与 bind 平移（欧几里得坐标），看 hand.R 在角色哪一侧
#include <cstdio>
#include "brf.h"
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: bone_dump <skeletons.brf>\n"); return 1; }
    try {
        brf::BrfFile bf = brf::parse(argv[1]);
        for (auto& s : bf.skeletons) {
            std::printf("skeleton[%s] bones=%zu\n", s.name.c_str(), s.bones.size());
            for (size_t i = 0; i < s.bones.size(); i++) {
                auto& b = s.bones[i];
                // bone 字段：attach + 可选 name + t(x,z,y 磁盘序→欧几里得 X=f0,Y=f2,Z=f1) + rot
                std::printf("  [%2zu] %-12s t=(% .3f, % .3f, % .3f)\n", i, b.name.c_str(), b.t.x, b.t.y, b.t.z);
            }
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "解析失败: %s\n", e.what()); return 1; }
    return 0;
}
