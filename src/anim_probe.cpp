// 诊断：dump 指定动画的前几帧里每根骨的旋转四元数（对比找挥刀骨）
#include <cstdio>
#include <cmath>
#include "brf.h"
int main(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: anim_probe <skeletons.brf> <ani.brf> <animName>\n"); return 1; }
    try {
        brf::BrfFile sk = brf::parse(argv[1]);
        brf::BrfFile an = brf::parse(argv[2]);
        brf::Skeleton* s = nullptr;
        for (auto& x : sk.skeletons) if (x.name == "skel_human") { s = &x; break; }
        brf::SkeletonAnim* a = nullptr;
        for (auto& x : an.anims) if (x.name == argv[3]) { a = &x; break; }
        if (!s || !a) { std::fprintf(stderr, "找不到骨架或动画\n"); return 1; }
        std::printf("anim[%s] frames=%zu bones=%zu\n", a->name.c_str(), a->frames.size(), s->bones.size());
        for (size_t f = 0; f < a->frames.size() && f < 6; f++) {
            auto& fr = a->frames[f];
            std::printf("frame %zu: ", f);
            for (size_t b = 0; b < s->bones.size() && b < fr.rots.size(); b++) {
                auto& q = fr.rots[b];
                float ang = 2.f * acosf(std::fmin(1.f, std::fabs(q.w))) * 180.f / 3.14159f;
                if (ang > 5.f) // 只打印转动超过 5° 的骨
                    std::printf("[%s %.0f°] ", s->bones[b].name.c_str(), ang);
            }
            std::printf("\n");
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "失败: %s\n", e.what()); return 1; }
    return 0;
}
