// dds.h —— 自研 DDS 解析器（DXT1/3/5 软件解码为 RGBA8）
// 仅依赖标准库；按公开 DDS 文件格式规范独立实现。
#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dds {

struct Image {
    uint32_t width = 0, height = 0;
    uint32_t mipCount = 1;
    std::vector<uint8_t> rgba; // 最顶层 mip，RGBA8
};

static inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

static inline uint16_t rgb565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
    g = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
    b = (uint8_t)((v & 0x1F) * 255 / 31);
    return v;
}

static inline void decodeDxt1Block(const uint8_t* blk, uint8_t* out, uint32_t rowPitch) {
    uint8_t r0,g0,b0,r1,g1,b1,r2,g2,b2,r3,g3,b3;
    uint16_t c0 = blk[0] | (blk[1]<<8), c1 = blk[2] | (blk[3]<<8);
    rgb565(c0, r0, g0, b0); rgb565(c1, r1, g1, b1);
    uint32_t bits = blk[4] | (blk[5]<<8) | (blk[6]<<16) | ((uint32_t)blk[7]<<24);
    bool fourColor = c0 > c1;
    if (fourColor) {
        r2 = (2*r0 + r1) / 3; g2 = (2*g0 + g1) / 3; b2 = (2*b0 + b1) / 3;
        r3 = (r0 + 2*r1) / 3; g3 = (g0 + 2*g1) / 3; b3 = (b0 + 2*b1) / 3;
    } else {
        r2 = (r0 + r1) / 2; g2 = (g0 + g1) / 2; b2 = (b0 + b1) / 2;
        r3 = g3 = b3 = 0;
    }
    for (int y = 0; y < 4; ++y) {
        uint8_t* row = out + y * rowPitch;
        for (int x = 0; x < 4; ++x) {
            uint8_t code = (bits >> ((y*4+x)*2)) & 3;
            uint8_t* px = row + x*4;
            switch (code) {
                case 0: px[0]=r0; px[1]=g0; px[2]=b0; px[3]=255; break;
                case 1: px[0]=r1; px[1]=g1; px[2]=b1; px[3]=255; break;
                case 2: px[0]=r2; px[1]=g2; px[2]=b2; px[3]=255; break;
                default: px[0]=r3; px[1]=g3; px[2]=b3; px[3]= fourColor ? 255 : 0; break;
            }
        }
    }
}

static inline void decodeDxt3Block(const uint8_t* blk, uint8_t* out, uint32_t rowPitch) {
    for (int y = 0; y < 4; ++y) {
        uint16_t a = blk[y*2] | (blk[y*2+1]<<8);
        uint8_t* row = out + y * rowPitch;
        for (int x = 0; x < 4; ++x) {
            uint8_t* px = row + x*4;
            px[3] = (uint8_t)(((a >> (x*4)) & 0xF) * 255 / 15);
        }
    }
    decodeDxt1Block(blk + 8, out, rowPitch); // DXT3 颜色块无透明分支
}

static inline void decodeDxt5Block(const uint8_t* blk, uint8_t* out, uint32_t rowPitch) {
    uint8_t a0 = blk[0], a1 = blk[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[2+i] << (i*8);
    for (int y = 0; y < 4; ++y) {
        uint8_t* row = out + y * rowPitch;
        for (int x = 0; x < 4; ++x) {
            uint8_t code = (bits >> ((y*4+x)*3)) & 7;
            uint8_t a;
            if (a0 > a1) {
                switch (code) { case 0: a=a0; break; case 1: a=a1; break;
                    case 2: a=(6*a0 + 1*a1)/7; break; case 3: a=(5*a0 + 2*a1)/7; break;
                    case 4: a=(4*a0 + 3*a1)/7; break; case 5: a=(3*a0 + 4*a1)/7; break;
                    case 6: a=(2*a0 + 5*a1)/7; break; default: a=(a0 + 6*a1)/7; break; }
            } else {
                switch (code) { case 0: a=a0; break; case 1: a=a1; break;
                    case 2: a=(4*a0 + 1*a1)/5; break; case 3: a=(3*a0 + 2*a1)/5; break;
                    case 4: a=(2*a0 + 3*a1)/5; break; case 5: a=(a0 + 4*a1)/5; break;
                    case 6: a=0; break; default: a=255; break; }
            }
            row[x*4+3] = a;
        }
    }
    decodeDxt1Block(blk + 8, out, rowPitch);
}

inline Image load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("DDS 打不开: " + path);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) throw std::runtime_error("不是 DDS 文件: " + path);
    const uint8_t* h = d.data() + 4;
    Image img;
    img.height = le32(h + 8);
    img.width = le32(h + 12);
    img.mipCount = le32(h + 24); if (img.mipCount == 0) img.mipCount = 1;
    uint32_t fourcc = le32(h + 80);
    if (img.width == 0 || img.height == 0 || img.width > 16384 || img.height > 16384)
        throw std::runtime_error("DDS 尺寸异常");
    uint32_t blockBytes = 0;
    if (fourcc == 0x31545844) blockBytes = 8;       // DXT1
    else if (fourcc == 0x33545844) blockBytes = 16; // DXT3
    else if (fourcc == 0x35545844) blockBytes = 16; // DXT5
    else throw std::runtime_error("暂不支持的 DDS 格式 fourcc=0x" + std::to_string(fourcc));
    img.rgba.resize((size_t)img.width * img.height * 4);
    const uint8_t* src = d.data() + 128;
    uint32_t bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
    uint32_t rowPitch = img.width * 4;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t* out = img.rgba.data() + ((size_t)by * 4 * rowPitch + (size_t)bx * 16);
            if (blockBytes == 8) decodeDxt1Block(src, out, rowPitch);
            else if (fourcc == 0x33545844) decodeDxt3Block(src, out, rowPitch);
            else decodeDxt5Block(src, out, rowPitch);
            src += blockBytes;
        }
    }
    return img;
}

} // namespace dds
