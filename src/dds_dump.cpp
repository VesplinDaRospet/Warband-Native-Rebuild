// dds_dump.cpp —— 解码 DDS 并写 BMP（验证解码器正确性的中间件）
#include "dds.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: dds_dump <in.dds> <out.bmp>\n"); return 1; }
    try {
        dds::Image img = dds::load(argv[1]);
        uint32_t w = img.width, h = img.height;
        std::printf("OK %ux%u mips=%u\n", w, h, img.mipCount);
        FILE* f = fopen(argv[2], "wb");
        if (!f) { std::printf("FAIL 打不开输出\n"); return 1; }
        uint32_t rowSize = w * 3;
        rowSize = (rowSize + 3) & ~3u;
        uint32_t dataSize = rowSize * h;
        uint32_t fileSize = 54 + dataSize;
        uint8_t hdr[54] = {};
        hdr[0]='B'; hdr[1]='M';
        memcpy(hdr+2, &fileSize, 4);
        uint32_t off=54; memcpy(hdr+10, &off, 4);
        uint32_t hs=40; memcpy(hdr+14, &hs, 4);
        memcpy(hdr+18, &w, 4); memcpy(hdr+22, &h, 4);
        uint16_t planes=1; memcpy(hdr+26, &planes, 2);
        uint16_t bpp=24; memcpy(hdr+28, &bpp, 2);
        memcpy(hdr+34, &dataSize, 4);
        fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> row(rowSize);
        for (int y = (int)h - 1; y >= 0; --y) {
            const uint8_t* src = img.rgba.data() + (size_t)y * w * 4;
            memset(row.data(), 0, rowSize);
            for (uint32_t x = 0; x < w; ++x) {
                row[x*3+0] = src[x*4+2]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+0];
            }
            fwrite(row.data(), 1, rowSize, f);
        }
        fclose(f);
        std::printf("WROTE %s\n", argv[2]);
        return 0;
    } catch (const std::exception& e) {
        std::printf("ERROR: %s\n", e.what());
        return 1;
    }
}
