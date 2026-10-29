// 【阅读导航】运动输入模型：维护投影米制位置；手动移动或自动往返只更新位置，区块切换交给 TileManager。
#pragma once
#include "veylaxis/vgds/Types.hpp"
#include <cmath>
#include <stdexcept>

namespace veylaxis::streaming
{
struct VesselMotion
{
    veylaxis::vgds::XY start, position;
    double speed{150}, tileSize{1000}, travel{};
    int east{}, north{};
    bool automatic{}, paused{}, follow{true};

    void advance(double dt)
    {
        if (!std::isfinite(dt) || dt < 0 || !std::isfinite(speed) || speed <= 0 || !std::isfinite(tileSize) ||
            tileSize <= 0)
            throw std::invalid_argument("Invalid motion parameters");
        if (paused)
            return;
        if (automatic)
        {
            travel = std::fmod(travel + speed * dt, 4 * tileSize);
            position = {start.x + (travel <= 2 * tileSize ? travel : 4 * tileSize - travel), start.y};
        }
        else
        {
            double length = std::hypot(east, north);
            if (length)
            {
                position.x += east / length * speed * dt;
                position.y += north / length * speed * dt;
            }
        }
    }
};
}
