// 快速诊断：dump brf 里全部骨架动画名与帧数
#include <cstdio>
#include "brf.h"
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: anim_dump <file.brf>\n"); return 1; }
    try {
        brf::BrfFile bf = brf::parse(argv[1]);
        std::printf("version=%d meshes=%zu skeletons=%zu anims=%zu\n",
                    bf.version, bf.meshes.size(), bf.skeletons.size(), bf.anims.size());
        for (auto& a : bf.anims) {
            std::printf("  anim[%s] frames=%zu\n", a.name.c_str(), a.frames.size());
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "解析失败: %s\n", e.what()); return 1; }
    return 0;
}
