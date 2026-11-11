// sco_dump.cpp —— 解析 .sco 场景物件并与 scene_props.txt 对照
#include "sco.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

struct Spr { int flags; std::string mesh, body; };

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: sco_dump <scene.sco> <scene_props.txt>\n"); return 1; }
    try {
        sco::Scene s = sco::load(argv[1]);
        // 解析 scene_props.txt（每条4行：主行 + 空行；主行字段: spr_id flags x mesh body 0）
        std::map<uint32_t, Spr> sprs;
        std::ifstream pf(argv[2]);
        std::string line; std::getline(pf, line); int n=0; pf >> n;
        uint32_t idx = 0;
        while (std::getline(pf, line)) {
            if (line.empty()) continue;
            if (line.rfind("spr_",0)!=0) continue;
            std::istringstream ss(line);
            std::string idstr; ss >> idstr;
            Spr spr; int x;
            ss >> spr.flags >> x >> spr.mesh >> spr.body >> x;
            sprs[idx] = spr;
            idx++;
        }
        std::printf("场景版本=%d 物件总数=%zu 场景道具定义=%zu\n", s.version, s.objects.size(), sprs.size());
        size_t nProp=0;
        for (auto& o : s.objects) {
            if (o.type==0) nProp++;
        }
        std::printf("Prop 实例=%zu\n", nProp);
        size_t shown=0;
        for (auto& o : s.objects) {
            if (o.type==0 && shown++<20) {
                auto it=sprs.find(o.id);
                std::printf("  prop id=%u [%s] mesh=%s pos=(%.2f %.2f %.2f) scale=(%.2f %.2f %.2f)\n",
                    o.id, it!=sprs.end()?("spr_"+std::to_string(o.id)).c_str():"?",
                    it!=sprs.end()?it->second.mesh.c_str():"?", o.pos[0],o.pos[1],o.pos[2],
                    o.scale[0],o.scale[1],o.scale[2]);
            }
        }
        return 0;
    } catch (const std::exception& e) { std::printf("ERROR: %s\n", e.what()); return 1; }
}
