// 【阅读导航】模拟源几何：构造跨区块、带孔及多连通区域，供裁剪、缓存和动态加载测试复用。
#include "veylaxis/vgds/Service.hpp"
#include <cmath>

// Formula ported from perevia-osgkit/tests/IslandExample.h; see
// third_party/notices/perevia-MIT.txt. The original engineering project is untouched.
namespace veylaxis::vgds
{
Dataset islandFixture(const GridSpec& grid)
{
    Dataset d;
    d.id = "mock-island80";
    d.version = "fixture-1";
    d.grid = grid;
    Polygon p;
    constexpr double tau = 6.283185307179586;
    const double x = grid.origin.x + grid.tileSizeMeters, y = grid.origin.y + grid.tileSizeMeters / 2;
    // Preserve the source shape/holes, uniformly enlarge 3x to cross the seam.
    for (unsigned i = 0; i < 80; ++i)
    {
        double a = tau * i / 80,
               r = 1 + .14 * std::sin(3 * a + .3) + .10 * std::sin(7 * a) + .045 * std::cos(13 * a);
        p.outer.points.push_back({x + 660 * r * std::cos(a), y + 510 * r * std::sin(a)});
    }
    p.outer.edges.assign(80, BoundaryKind::Geographic);
    auto hole = [&](unsigned count, double hx, double hy, double rx, double ry, double phase)
    {
        Ring h;
        for (unsigned i = 0; i < count; ++i)
        {
            double a = tau * i / count, r = 1 + .13 * std::sin(3 * a + phase);
            h.points.push_back({x + 3 * (hx + rx * r * std::cos(a)), y + 3 * (hy + ry * r * std::sin(a))});
        }
        h.edges.assign(count, BoundaryKind::Geographic);
        p.holes.push_back(std::move(h));
    };
    hole(20, -65, 10, 44, 32, .2);
    hole(16, 65, -25, 28, 23, .7);
    Feature f;
    f.layer = "land";
    f.objectType = "SYNTHETIC_ISLAND";
    f.sourceObjectId = "mock:island80";
    f.fragmentId = "source";
    f.dataVersion = d.version;
    f.geometry = p;
    d.features.push_back(std::move(f));
    return d;
}
}
