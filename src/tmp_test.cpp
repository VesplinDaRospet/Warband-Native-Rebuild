// BRF 读取连通性自检：tmp_test.exe [foo.brf]，默认当前目录 arena.brf
#include "brf.h"
#include <cstdio>
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "arena.brf";
    try {
        brf::BrfFile b = brf::parse(path, true);
        printf("meshes=%zu tex=%zu mat=%zu unknown=%zu\n", b.meshes.size(), b.textures.size(), b.materials.size(), b.unknownTokens.size());
        for (size_t i=0;i<b.meshes.size() && i<5;i++)
            printf("mesh[%s] pos=%zu verts=%zu tris=%zu\n", b.meshes[i].name.c_str(), b.meshes[i].positions.size(), b.meshes[i].verts.size(), b.meshes[i].indices.size()/3);
        return 0;
    } catch (const std::exception& e) { printf("ERR %s/n", e.what()); return 1; }
}
