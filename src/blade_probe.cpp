// 诊断：试握持 fix 候选——刃朝前(局部+z)、斧柄与手指方向(骨+x)垂直（临时工具）
#include <cstdio>
#include "brf.h"
#include "skin.h"

static void probe(const brf::Skeleton& skel, const brf::SkeletonAnim& anim, int hr,
                  const char* label, const skin::M44& fix) {
    std::printf("--- %s ---\n", label);
    for (size_t fi = 0; fi < anim.frames.size(); fi += 8) {
        std::vector<skin::M44> ag;
        skin::animGlobals(skel, anim.frames[fi], ag);
        skin::M44 M = skin::mul(ag[hr], fix);
        brf::Vec3 blade{1, 0, 0}; skin::xformDir(M, blade); // part x = 刃
        brf::Vec3 haft{0, 0, 1}; skin::xformDir(M, haft);  // part z = 斧柄
        brf::Vec3 finger{1, 0, 0}; skin::xformDir(ag[hr], finger); // 手指方向 = 骨+x
        float dot = haft.x * finger.x + haft.y * finger.y + haft.z * finger.z; // 斧柄·手指 → 0=垂直
        std::printf("f%2zu blade z=% .2f | haft=(% .2f,% .2f,% .2f) finger=(% .2f,% .2f,% .2f) dot=% .2f\n",
                    fi, blade.z, haft.x, haft.y, haft.z, finger.x, finger.y, finger.z, dot);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: blade_probe <skeletons.brf> <ani_attacks.brf>\n"); return 1; }
    try {
        brf::BrfFile skf = brf::parse(argv[1]);
        brf::BrfFile atf = brf::parse(argv[2]);
        brf::Skeleton* skel = nullptr;
        for (auto& s : skf.skeletons) if (s.name == "skel_human") skel = &s;
        int hr = -1;
        for (size_t i = 0; i < skel->bones.size(); i++) if (skel->bones[i].name == "hand.R") hr = (int)i;
        // fixA: x→骨+x, y→骨-y, z→骨+z（刃朝手指方向，斧柄垂直手指，斧柄朝+骨z）
        skin::M44 fa = skin::identity();
        fa.m[0] = 1; fa.m[1] = 0; fa.m[2] = 0;
        fa.m[4] = 0; fa.m[5] = -1; fa.m[6] = 0;
        fa.m[8] = 0; fa.m[9] = 0; fa.m[10] = 1;
        // fixB: 同 A 但 z→骨-z（斧柄反方向）
        skin::M44 fb = fa;
        fb.m[8] = 0; fb.m[9] = 0; fb.m[10] = -1;
        for (auto& a : atf.anims) {
            if (a.name != "attacks_single_lefttoright" && a.name != "attacks_single_overswing") continue;
            std::printf("===== %s (%zu帧) =====\n", a.name.c_str(), a.frames.size());
            probe(*skel, a, hr, "fixA x→骨x y→骨-y z→骨z", fa);
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "失败: %s\n", e.what()); return 1; }
    return 0;
}
