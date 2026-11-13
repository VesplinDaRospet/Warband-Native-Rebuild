// 诊断：算 skel_human 每根骨的 bind 全局位置（蒙皮欧几里得坐标），看 hand.R 的 x 符号
#include <cstdio>
#include "brf.h"
#include "skin.h"
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: glob_bone <skeletons.brf>\n"); return 1; }
    try {
        brf::BrfFile bf = brf::parse(argv[1]);
        for (auto& s : bf.skeletons) {
            if (s.name != "skel_human") continue;
            std::vector<skin::M44> bind;
            skin::bindGlobals(s, bind);
            std::printf("skeleton[%s] bones=%zu\n", s.name.c_str(), s.bones.size());
            for (size_t i = 0; i < s.bones.size(); i++) {
                brf::Vec3 o{0, 0, 0};
                skin::xformPos(bind[i], o);
                std::printf("  [%2zu] %-12s global=(% .3f, % .3f, % .3f)\n", i, s.bones[i].name.c_str(), o.x, o.y, o.z);
            }
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "失败: %s\n", e.what()); return 1; }
    return 0;
}
