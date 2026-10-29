// 【阅读导航】先读此文件：VGDS 的公共数据契约，只有 STL 类型，不含 OSG/GDAL/Qt。
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <array>

namespace veylaxis::vgds
{
inline constexpr int SchemaVersion = 1;

struct XY
{
    double x{}, y{};
};

struct Bounds
{
    double minX{}, minY{}, maxX{}, maxY{};
};

// CRS is an opaque identifier supplied by the caller; no CRS conversion here.
// 网格原点不是经纬度；当前默认 EPSG:32619，坐标值为投影东向/北向米数。
struct GridSpec
{
    std::string id, horizontalCrs;
    XY origin;
    double tileSizeMeters{};
    std::string horizontalUnit{"m"};
};

struct TileID
{
    std::string gridId;
    std::int64_t x{}, y{};
};

inline bool operator==(const TileID& a, const TileID& b)
{
    return a.gridId == b.gridId && a.x == b.x && a.y == b.y;
}
// Geographic 才是真实边界；ArtificialClip 是切块产生的边，不能拿来建海岸侧壁。
enum class BoundaryKind
{
    Unknown,
    Geographic,
    ArtificialClip
};

struct Point
{
    XY position;
};

struct LineString
{
    std::vector<XY> points;
    std::vector<BoundaryKind> edges;
};

// Implicitly closed ring: no repeated final point; edges[i] describes i -> (i+1)%N.
struct Ring
{
    std::vector<XY> points;
    std::vector<BoundaryKind> edges;
};

struct Polygon
{
    Ring outer;
    std::vector<Ring> holes;
};

struct MultiPolygon
{
    std::vector<Polygon> polygons;
};

using Geometry = std::variant<Point, LineString, Polygon, MultiPolygon>;
using Property =
    std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>>;
enum class Provenance
{
    Observed,
    Estimated,
    Synthetic
};
enum class LayerState
{
    Available,
    Empty,
    NoCoverage,
    Unsupported
};

// sourceObjectId 标识原始物标，fragmentId 标识裁剪片段，dataVersion 区分源内容版本。
struct Feature
{
    std::string layer, objectType, sourceObjectId, fragmentId, dataVersion;
    Provenance provenance{Provenance::Synthetic};
    Geometry geometry;
    std::map<std::string, Property> properties;
};

// 海底高程 Z 向上，水下通常为负；valid=0 表示无数据，不应当填成海平面。
struct TerrainData
{
    std::uint32_t columns{}, rows{};
    XY northWestSample;
    double spacingMeters{};
    std::string verticalDatum;
    Provenance provenance{Provenance::Synthetic};
    // Row-major, columns +X, rows -Y; elevation metres +Z up, not positive depth.
    // Invalid samples decode to quiet NaN with valid[i]==0; wire value is null.
    // Consumers must use the mask; missing data is never a sea-level zero.
    std::vector<float> elevations;
    std::vector<std::uint8_t> valid;
    // Optional global-field normals, never estimated independently at tile edges.
    std::vector<std::array<float, 3>> normals;
};

struct TileRequest
{
    std::string requestId;
    TileID tile;
    std::vector<std::string> layers;
    double terrainSpacingMeters{10.0};
    int schemaVersion{SchemaVersion};
};

struct TileMetadata
{
    int schemaVersion{SchemaVersion};
    std::string requestId, datasetId, dataVersion, algorithmVersion;
    GridSpec grid;
    TileID tile;
    Bounds bounds;
    XY tileOrigin; // southwest logical tile origin, global projected metres
    Provenance provenance{Provenance::Synthetic};
    std::map<std::string, LayerState> layers;
};

// Optional, regenerable rendering cache. Positions inside the file are tile-local
// metres; the client places the node at origin. Geographic features remain primary.
struct RenderArtifact
{
    std::string format{"osgb"}, path, builderVersion, contentHash;
    XY origin;
    double zScale{1};
    std::uint64_t bytes{};
};

// 一次 Tile 响应：语义要素为主，可选高程采样和可重新生成的静态渲染文件。
struct TilePackage
{
    TileMetadata metadata;
    std::vector<Feature> features;
    std::optional<TerrainData> terrain;
    std::vector<RenderArtifact> artifacts;
};
enum class ErrorCode
{
    InvalidRequest,
    InvalidGrid,
    InvalidResolution,
    UnsupportedVersion,
    InvalidPackage,
    InvalidEncoding,
    IoFailure,
    GenerationFailure,
    Cancelled
};

struct TileError
{
    ErrorCode code;
    std::string message;
};
template <class T> using Result = std::variant<T, TileError>;
using TileResult = Result<TilePackage>;
}
