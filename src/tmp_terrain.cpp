#include "sco.h"
#include <cstdio>
#include <algorithm>
#include <map>
int main(int argc, char** argv) {
    const char* path = argc>1 ? argv[1] : "scn_town_1_center.sco";
    try {
        sco::Terrain t;
        bool ok = sco::loadTerrain(path, t, false);
        printf("ok=%d w=%u h=%u elevation=%zu layers=%zu\n", ok, t.width, t.height, t.elevation.size(), t.layers.size());
        if (!t.elevation.empty()) {
            float mn=1e9f,mx=-1e9f; for (auto h:t.elevation){mn=std::min(mn,h);mx=std::max(mx,h);}
            printf("height min=%.2f max=%.2f\n", mn, mx);
        }
        for (auto& l : t.layers) {
            size_t nz = 0; unsigned mxv = 0;
            for (auto v : l.cells) { if (v) nz++; mxv = std::max(mxv, (unsigned)v); }
            size_t nc = 0; for (auto c : l.colors) if (c & 0xFFFFFF) nc++;
            std::map<unsigned,int> hist; for (auto v : l.cells) hist[v]++;
            std::string hh; int shown=0;
            for (auto& kv : hist) { if (shown++>=6) break; hh += std::to_string(kv.first) + ":" + std::to_string(kv.second) + " "; }
            printf("layer[%d] %-20s en=%u cells=%zu nz=%zu max=%u hist={%s} colors=%zu/%zu\n",
                   l.index, l.name.c_str(), l.enabled, l.cells.size(), nz, mxv, hh.c_str(), nc, l.colors.size());
        }
        sco::Scene s = sco::load(path);
        float mnx=1e9f,mnz=1e9f,mxx=-1e9f,mxz=-1e9f;
        size_t np=0;
        for (auto& o : s.objects) if (o.type==0) {
            np++;
            mnx=std::min(mnx,o.pos[0]); mxx=std::max(mxx,o.pos[0]);
            mnz=std::min(mnz,o.pos[2]); mxz=std::max(mxz,o.pos[2]);
        }
        printf("props=%zu bbox x=[%.2f,%.2f] z=[%.2f,%.2f] size=(%.2f,%.2f)\n",
               np, mnx, mxx, mnz, mxz, mxx-mnx, mxz-mnz);
        printf("grid cells=(%u,%u) => cell size if props-bound: x=%.3f z=%.3f\n",
               t.width, t.height, (mxx-mnx)/(t.width-1), (mxz-mnz)/(t.height-1));
        return 0;
    } catch (const std::exception& e) { printf("ERR %s\n", e.what()); return 1; }
}
