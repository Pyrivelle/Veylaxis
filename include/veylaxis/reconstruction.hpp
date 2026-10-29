// 【阅读导航】海底算法替换入口：ISeabedReconstructor 返回正水深估计，投影、遮罩和高程符号由调用链处理。
#pragma once
#include <optional>
#include <vector>

namespace veylaxis
{
struct XY
{
    double x{}, y{};
};

struct Segment
{
    XY a, b;
    double depth{};
};

struct Sounding
{
    XY position;
    double depth{};
    int area{-1};
};

struct DepthBand
{
    double lower{}, upper{};
};

struct Estimate
{
    double depth{};
    int quality{};
}; // 0: both contours; 1: one contour

struct ReconstructionInput
{
    std::vector<Segment> contours;
    std::vector<Sounding> soundings;
    std::vector<DepthBand> bands;
    // Optional per-polygon adjacency: indices into contours. Empty uses all
    // segments (useful for synthetic algorithms/tests without polygon geometry).
    std::vector<std::vector<size_t>> areaContourIndices;
    double soundingRadius{250.0};
};

// All coordinates/distances are projected metres. Depth is positive downward.
// The pipeline alone handles masking, CRS and conversion to negative elevation.
class ISeabedReconstructor
{
  public:
    virtual ~ISeabedReconstructor() = default;
    virtual std::optional<Estimate> evaluate(XY position, int area) const = 0;
};

class LinearContourReconstructor final : public ISeabedReconstructor
{
  public:
    explicit LinearContourReconstructor(ReconstructionInput input);
    std::optional<Estimate> evaluate(XY position, int area) const override;

  private:
    struct Base
    {
        double depth, taper;
        int quality;
    };

    std::optional<Base> base(XY position, int area) const;
    ReconstructionInput input_;
    std::vector<double> residuals_;
};

double segmentDistance(XY point, const Segment& segment);
// Repair only explicitly contradictory DEPARE shared edges. This postprocess
// relaxes depth bounds inside a marked transition strip; it never fills NoData.
int repairDepthAreaConflicts(std::vector<float>& elevations, std::vector<unsigned char>& quality, int width,
                             int height, XY upperLeft, double cellSize, const std::vector<Segment>& conflicts,
                             double radius = 100.0, double coreRadius = 15.0);
}
