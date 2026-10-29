#include "veylaxis/reconstruction.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace veylaxis;

namespace
{
void check(bool b, const char* message)
{
    if (!b)
        throw std::runtime_error(message);
}

void near(double a, double b)
{
    check(std::abs(a - b) < 1e-6, "Unexpected interpolation result");
}
}
int runReconstructionTests()
try
{
    ReconstructionInput i;
    i.bands = {{10, 20}, {0, 10}, {100, 200}};
    i.contours = {{{0, -100}, {0, 100}, 10}, {{100, -100}, {100, 100}, 20}};
    i.soundingRadius = 40;
    LinearContourReconstructor flat(i);
    near(flat.evaluate({0, 0}, 0)->depth, 10);
    near(flat.evaluate({100, 0}, 0)->depth, 20);
    near(flat.evaluate({25, 0}, 0)->depth, 12.5);
    near(flat.evaluate({75, 0}, 0)->depth, 17.5);
    check(!flat.evaluate({10, 0}, 2), "Unsupported band must be NoData");
    check(!flat.evaluate({10, 0}, -1), "Outside mask must be NoData");
    check(flat.evaluate({10, 0}, 1)->quality == 1, "Missing boundary must be flagged");
    i.soundings = {{{50, 0}, 18, 0}};
    LinearContourReconstructor withPoint(i);
    near(withPoint.evaluate({50, 0}, 0)->depth, 18);
    near(withPoint.evaluate({0, 0}, 0)->depth, 10);
    near(withPoint.evaluate({100, 0}, 0)->depth, 20);
    near(withPoint.evaluate({50, 50}, 0)->depth, 15);
    check(std::abs(withPoint.evaluate({50, 0.001}, 0)->depth - 18) < 1e-5, "Sounding continuity");
    near(withPoint.evaluate({50, 40}, 0)->depth, 15);
    check(std::abs(withPoint.evaluate({50, 39.999}, 0)->depth - 15) < 1e-6, "Compact support continuity");
    near(segmentDistance({5, 3}, {{0, 0}, {10, 0}, 0}), 3);
    near(segmentDistance({12, 0}, {{0, 0}, {10, 0}, 0}), 2);
    near(segmentDistance({0, 3}, {{0, 0}, {0, 0}, 0}), 3);
    // Correction cannot escape the enclosing DEPARE depth range.
    i.soundings = {{{50, 0}, 200, 0}};
    LinearContourReconstructor extreme(i);
    near(extreme.evaluate({50, 0}, 0)->depth, 20);
    ReconstructionInput adjacency = i;
    adjacency.areaContourIndices = {{0}, {0}, {}};
    adjacency.soundings.clear();
    LinearContourReconstructor separated(adjacency);
    check(separated.evaluate({50, 0}, 0)->quality == 1, "Unrelated contour must not bracket this area");
    const int w = 60, h = 20;
    std::vector<float> grid(w * h);
    std::vector<unsigned char> quality(w * h, 1);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            grid[r * w + c] = c < 30 ? -10.f : -30.f;
    grid[0] = -9999;
    quality[0] = 0;
    const std::vector<Segment> conflict = {{{150, 0}, {150, -100}, 0}};
    check(repairDepthAreaConflicts(grid, quality, w, h, {0, 0}, 5, conflict, 50, 10) > 0,
          "Conflict was not repaired");
    check(grid[0] == -9999 && quality[0] == 0, "Repair changed NoData");
    check(grid[5 * w + 1] == -10 && quality[5 * w + 1] == 1, "Repair escaped strip");
    for (int c = 1; c < w - 1; ++c)
        check(std::abs(grid[10 * w + c] - grid[10 * w + c + 1]) < 4, "Repair left a discontinuity");
    std::cout << "Interpolation, constraints, missing support and continuity checks passed\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
