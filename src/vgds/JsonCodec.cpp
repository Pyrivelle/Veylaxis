// 【阅读导航】纯 STL 数据的 JSON 协议实现：编码前和解码后都校验结构、坐标、来源身份及栅格掩码。
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace veylaxis::vgds
{
namespace
{
using J = nlohmann::json;

void require(bool b, const char* message)
{
    if (!b)
        throw std::invalid_argument(message);
}

J parseBounded(const std::string& encoded)
{
    require(encoded.size() <= 16 * 1024 * 1024, "JSON exceeds 16MiB limit");
    std::vector<std::set<std::string>> keys;
    return J::parse(encoded,
                    [&](int depth, J::parse_event_t event, J& parsed)
                    {
                        require(depth <= 32, "JSON nesting exceeds limit");
                        if (event == J::parse_event_t::object_start)
                            keys.emplace_back();
                        else if (event == J::parse_event_t::key)
                            require(!keys.empty() && keys.back().insert(parsed.get<std::string>()).second,
                                    "Duplicate JSON key");
                        else if (event == J::parse_event_t::object_end)
                            keys.pop_back();
                        return true;
                    });
}

double number(const J& j)
{
    require(j.is_number(), "Expected numeric value");
    double v = j.get<double>();
    require(std::isfinite(v), "Nonfinite number");
    return v;
}

std::int64_t integer(const J& j)
{
    require(j.is_number_integer(), "Expected integer");
    if (j.is_number_unsigned())
        require(j.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX), "Integer overflow");
    return j.get<std::int64_t>();
}

template <class E> E enumeration(const J& j, int max)
{
    auto v = integer(j);
    require(v >= 0 && v <= max, "Unknown enumeration");
    return static_cast<E>(v);
}

std::string text(const J& j)
{
    require(j.is_string(), "Expected string");
    auto s = j.get<std::string>();
    require(s.size() <= 1048576, "String too large");
    return s;
}

void array(const J& j, std::size_t max = 1048576)
{
    require(j.is_array() && j.size() <= max, "Invalid/oversized array");
}

J xy(XY p)
{
    return J::array({p.x, p.y});
}

XY xy(const J& j)
{
    array(j, 2);
    require(j.size() == 2, "XY requires two ordinates");
    return {number(j[0]), number(j[1])};
}

J ring(const Ring& r)
{
    J p = J::array(), e = J::array();
    for (auto v : r.points)
        p.push_back(xy(v));
    for (auto v : r.edges)
        e.push_back(static_cast<int>(v));
    return {{"points", p}, {"edges", e}};
}

Ring ring(const J& j)
{
    Ring r;
    array(j.at("points"));
    array(j.at("edges"));
    for (const auto& p : j.at("points"))
        r.points.push_back(xy(p));
    for (const auto& e : j.at("edges"))
        r.edges.push_back(enumeration<BoundaryKind>(e, 2));
    return r;
}

J polygon(const Polygon& p)
{
    J h = J::array();
    for (const auto& r : p.holes)
        h.push_back(ring(r));
    return {{"outer", ring(p.outer)}, {"holes", h}};
}

Polygon polygon(const J& j)
{
    Polygon p;
    p.outer = ring(j.at("outer"));
    array(j.at("holes"));
    for (const auto& h : j.at("holes"))
        p.holes.push_back(ring(h));
    return p;
}

J geometry(const Geometry& g)
{
    return std::visit(
        [](const auto& value) -> J
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Point>)
                return {{"type", "point"}, {"position", xy(value.position)}};
            else if constexpr (std::is_same_v<T, LineString>)
            {
                auto j = ring(Ring{value.points, value.edges});
                j["type"] = "line";
                return j;
            }
            else if constexpr (std::is_same_v<T, Polygon>)
            {
                auto j = polygon(value);
                j["type"] = "polygon";
                return j;
            }
            else
            {
                J a = J::array();
                for (const auto& p : value.polygons)
                    a.push_back(polygon(p));
                return {{"type", "multipolygon"}, {"polygons", a}};
            }
        },
        g);
}

Geometry geometry(const J& j)
{
    auto type = text(j.at("type"));
    if (type == "point")
        return Point{xy(j.at("position"))};
    if (type == "line")
    {
        auto r = ring(j);
        return LineString{r.points, r.edges};
    }
    if (type == "polygon")
        return polygon(j);
    if (type == "multipolygon")
    {
        MultiPolygon m;
        array(j.at("polygons"));
        for (const auto& p : j.at("polygons"))
            m.polygons.push_back(polygon(p));
        return m;
    }
    throw std::invalid_argument("Unknown geometry type");
}

J property(const Property& p)
{
    J value;
    std::visit(
        [&](const auto& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (!std::is_same_v<T, std::monostate>)
                value = v;
        },
        p);
    return {{"kind", p.index()}, {"value", value}};
}

Property property(const J& j)
{
    const auto k = integer(j.at("kind"));
    const auto& v = j.at("value");
    switch (k)
    {
    case 0:
        require(v.is_null(), "Invalid null property");
        return std::monostate{};
    case 1:
        require(v.is_boolean(), "Expected boolean property");
        return v.get<bool>();
    case 2:
        return integer(v);
    case 3:
        return number(v);
    case 4:
        return text(v);
    case 5:
    {
        array(v);
        std::vector<std::string> a;
        for (const auto& s : v)
            a.push_back(text(s));
        return a;
    }
    default:
        throw std::invalid_argument("Unknown property kind");
    }
}

bool finite(XY p)
{
    return std::isfinite(p.x) && std::isfinite(p.y);
}

void points(const std::vector<XY>& p, const Bounds& b)
{
    require(p.size() <= 1048576, "Too many geometry coordinates");
    for (auto v : p)
        require(finite(v) && v.x >= b.minX && v.x <= b.maxX && v.y >= b.minY && v.y <= b.maxY,
                "Feature outside logical tile bounds");
}

void edges(const std::vector<BoundaryKind>& e, std::size_t count)
{
    require(e.size() == count, "Boundary labels must cover every edge");
    for (auto v : e)
        require(static_cast<int>(v) >= 0 && static_cast<int>(v) <= 2, "Invalid boundary label");
}

void checkRing(const Ring& r, const Bounds& b)
{
    require(r.points.size() >= 3, "Ring requires three points");
    points(r.points, b);
    edges(r.edges, r.points.size());
    require(r.points.front().x != r.points.back().x || r.points.front().y != r.points.back().y,
            "Ring closure is implicit");
}

void checkPolygon(const Polygon& p, const Bounds& b)
{
    checkRing(p.outer, b);
    require(p.holes.size() <= 1024, "Too many holes");
    for (const auto& h : p.holes)
        checkRing(h, b);
}

void provenance(Provenance p)
{
    require(static_cast<int>(p) >= 0 && static_cast<int>(p) <= 2, "Invalid provenance");
}

void boundedText(const std::string& s)
{
    require(s.size() <= 1048576, "String exceeds contract limit");
}
}

std::optional<TileError> validate(const TilePackage& p)
{
    try
    {
        const auto& m = p.metadata;
        if (m.schemaVersion != SchemaVersion)
            return TileError{ErrorCode::UnsupportedVersion, "Unsupported package schema"};
        auto expected = boundsOf(m.grid, m.tile);
        if (auto e = std::get_if<TileError>(&expected))
            return *e;
        auto b = std::get<Bounds>(expected);
        require(m.bounds.minX == b.minX && m.bounds.minY == b.minY && m.bounds.maxX == b.maxX &&
                    m.bounds.maxY == b.maxY && m.tileOrigin.x == b.minX && m.tileOrigin.y == b.minY,
                "Metadata bounds/origin do not match grid and TileID");
        require(!m.datasetId.empty() && !m.dataVersion.empty() && !m.algorithmVersion.empty(),
                "Missing dataset/data/algorithm identity");
        provenance(m.provenance);
        for (const auto* s : {&m.requestId, &m.datasetId, &m.dataVersion, &m.algorithmVersion})
            boundedText(*s);
        require(p.artifacts.size() <= 4, "Too many rendering artifacts");
        for (const auto& a : p.artifacts)
        {
            require(a.format == "osgb" && !a.path.empty() && !a.builderVersion.empty() &&
                        !a.contentHash.empty() && a.bytes > 0,
                    "Invalid rendering artifact");
            require(a.origin.x == m.tileOrigin.x && a.origin.y == m.tileOrigin.y && a.zScale == 1,
                    "Invalid rendering artifact coordinates");
            for (const auto* s : {&a.path, &a.builderVersion, &a.contentHash})
                boundedText(*s);
        }
        std::vector<std::string> names;
        for (const auto& entry : m.layers)
        {
            names.push_back(entry.first);
            require(static_cast<int>(entry.second) >= 0 && static_cast<int>(entry.second) <= 3,
                    "Invalid layer state");
        }
        auto normalized = normalizeLayers(names);
        require(std::holds_alternative<std::vector<std::string>>(normalized), "Invalid layer set");
        require(p.features.size() <= 100000, "Too many features");
        std::set<std::string> fragments;
        std::map<std::string, std::size_t> counts;
        for (const auto& f : p.features)
        {
            require(!f.objectType.empty() && !f.sourceObjectId.empty() && !f.fragmentId.empty() &&
                        !f.dataVersion.empty(),
                    "Missing feature identity");
            for (const auto* s : {&f.objectType, &f.sourceObjectId, &f.fragmentId, &f.dataVersion})
                boundedText(*s);
            require(f.layer != "terrain" && m.layers.count(f.layer) &&
                        m.layers.at(f.layer) == LayerState::Available,
                    "Feature in unrequested/unavailable layer");
            require(fragments.insert(f.fragmentId).second, "Duplicate fragment identity");
            ++counts[f.layer];
            provenance(f.provenance);
            std::visit(
                [&](const auto& g)
                {
                    using T = std::decay_t<decltype(g)>;
                    if constexpr (std::is_same_v<T, Point>)
                    {
                        points({g.position}, b);
                        auto t = locate(m.grid, g.position);
                        require(std::holds_alternative<TileID>(t) && std::get<TileID>(t) == m.tile,
                                "Point violates half-open ownership");
                    }
                    else if constexpr (std::is_same_v<T, LineString>)
                    {
                        require(g.points.size() >= 2, "Line requires two points");
                        points(g.points, b);
                        edges(g.edges, g.points.size() - 1);
                    }
                    else if constexpr (std::is_same_v<T, Polygon>)
                        checkPolygon(g, b);
                    else
                    {
                        require(!g.polygons.empty() && g.polygons.size() <= 1024, "Invalid multipolygon");
                        for (const auto& v : g.polygons)
                            checkPolygon(v, b);
                    }
                },
                f.geometry);
            require(f.properties.size() <= 4096, "Too many attributes");
            for (const auto& entry : f.properties)
            {
                require(!entry.first.empty(), "Empty attribute name");
                boundedText(entry.first);
                if (auto v = std::get_if<double>(&entry.second))
                    require(std::isfinite(*v), "Nonfinite attribute");
                if (auto v = std::get_if<std::string>(&entry.second))
                    boundedText(*v);
                if (auto v = std::get_if<std::vector<std::string>>(&entry.second))
                {
                    require(v->size() <= 1048576, "Oversized string array");
                    for (const auto& s : *v)
                        boundedText(s);
                }
            }
        }
        for (const auto& entry : m.layers)
            if (entry.first != "terrain" && entry.second == LayerState::Available)
                require(counts[entry.first] > 0, "Available feature layer has no features; use Empty");
        if (p.terrain)
        {
            const auto& t = *p.terrain;
            require(m.layers.count("terrain"), "Unrequested terrain payload");
            auto n = nodeCount(m.grid, t.spacingMeters);
            require(std::holds_alternative<std::uint32_t>(n), "Invalid terrain spacing");
            auto side = std::get<std::uint32_t>(n);
            require(t.columns == side && t.rows == side && t.northWestSample.x == b.minX &&
                        t.northWestSample.y == b.maxY,
                    "Invalid shared-node terrain layout");
            require(!t.verticalDatum.empty(), "Missing vertical reference");
            boundedText(t.verticalDatum);
            provenance(t.provenance);
            const auto size = static_cast<std::size_t>(side) * side;
            require(t.elevations.size() == size && t.valid.size() == size,
                    "Terrain array/mask size mismatch");
            require(t.normals.empty() || t.normals.size() == size, "Terrain normal size mismatch");
            for (const auto& normal : t.normals)
            {
                double length = 0;
                for (float component : normal)
                {
                    require(std::isfinite(component), "Nonfinite terrain normal");
                    length += component * component;
                }
                require(std::abs(length - 1) < 1e-4, "Terrain normal is not normalized");
            }
            std::size_t valid = 0;
            for (std::size_t i = 0; i < size; ++i)
            {
                require(t.valid[i] <= 1, "Mask must contain 0/1");
                if (t.valid[i])
                {
                    require(std::isfinite(t.elevations[i]), "Valid elevation must be finite");
                    ++valid;
                }
            }
            require(m.layers.at("terrain") == (valid ? LayerState::Available : LayerState::NoCoverage),
                    "Terrain coverage and layer state disagree");
        }
        else if (m.layers.count("terrain"))
            require(m.layers.at("terrain") != LayerState::Available, "Missing available terrain payload");
        return {};
    }
    catch (const std::exception& e)
    {
        return TileError{ErrorCode::InvalidPackage, e.what()};
    }
}

Result<std::string> encodeJson(const TilePackage& p)
{
    if (auto e = validate(p))
        return *e;
    try
    {
        const auto& m = p.metadata;
        J layers = J::object();
        for (const auto& l : m.layers)
            layers[l.first] = static_cast<int>(l.second);
        J j = {{"schema", m.schemaVersion},
               {"requestId", m.requestId},
               {"datasetId", m.datasetId},
               {"dataVersion", m.dataVersion},
               {"algorithmVersion", m.algorithmVersion},
               {"grid",
                {{"id", m.grid.id},
                 {"crs", m.grid.horizontalCrs},
                 {"origin", xy(m.grid.origin)},
                 {"size", m.grid.tileSizeMeters},
                 {"unit", m.grid.horizontalUnit}}},
               {"tile", {{"gridId", m.tile.gridId}, {"x", m.tile.x}, {"y", m.tile.y}}},
               {"bounds", J::array({m.bounds.minX, m.bounds.minY, m.bounds.maxX, m.bounds.maxY})},
               {"tileOrigin", xy(m.tileOrigin)},
               {"provenance", static_cast<int>(m.provenance)},
               {"layers", layers},
               {"features", J::array()},
               {"terrain", nullptr}};
        for (const auto& f : p.features)
        {
            J attributes = J::object();
            for (const auto& entry : f.properties)
                attributes[entry.first] = property(entry.second);
            j["features"].push_back({{"layer", f.layer},
                                     {"objectType", f.objectType},
                                     {"sourceId", f.sourceObjectId},
                                     {"fragmentId", f.fragmentId},
                                     {"dataVersion", f.dataVersion},
                                     {"provenance", static_cast<int>(f.provenance)},
                                     {"geometry", geometry(f.geometry)},
                                     {"properties", attributes}});
        }
        if (p.terrain)
        {
            const auto& t = *p.terrain;
            J values = J::array();
            for (std::size_t i = 0; i < t.valid.size(); ++i)
            {
                if (t.valid[i])
                    values.push_back(t.elevations[i]);
                else
                    values.push_back(nullptr);
            }
            j["terrain"] = {{"columns", t.columns},
                            {"rows", t.rows},
                            {"northWestSample", xy(t.northWestSample)},
                            {"spacing", t.spacingMeters},
                            {"verticalDatum", t.verticalDatum},
                            {"provenance", static_cast<int>(t.provenance)},
                            {"elevations", values},
                            {"valid", t.valid}};
            if (!t.normals.empty())
                j["terrain"]["normals"] = t.normals;
        }
        if (!p.artifacts.empty())
        {
            j["artifacts"] = J::array();
            for (const auto& a : p.artifacts)
                j["artifacts"].push_back({{"format", a.format},
                                          {"path", a.path},
                                          {"builderVersion", a.builderVersion},
                                          {"hash", a.contentHash},
                                          {"origin", xy(a.origin)},
                                          {"zScale", a.zScale},
                                          {"bytes", a.bytes}});
        }
        auto encoded = j.dump();
        require(encoded.size() <= 16 * 1024 * 1024, "Package exceeds 16MiB JSON limit");
        return encoded;
    }
    catch (const std::exception& e)
    {
        return TileError{ErrorCode::InvalidEncoding, e.what()};
    }
}

TileResult decodeJson(const std::string& encoded)
{
    try
    {
        auto j = parseBounded(encoded);
        auto schema = integer(j.at("schema"));
        if (schema != SchemaVersion)
            return TileError{ErrorCode::UnsupportedVersion, "Unsupported package schema"};
        TilePackage p;
        auto& m = p.metadata;
        m.requestId = text(j.at("requestId"));
        m.datasetId = text(j.at("datasetId"));
        m.dataVersion = text(j.at("dataVersion"));
        m.algorithmVersion = text(j.at("algorithmVersion"));
        const auto& g = j.at("grid");
        m.grid = {text(g.at("id")), text(g.at("crs")), xy(g.at("origin")), number(g.at("size"))};
        m.grid.horizontalUnit = text(g.at("unit"));
        const auto& tile = j.at("tile");
        m.tile = {text(tile.at("gridId")), integer(tile.at("x")), integer(tile.at("y"))};
        const auto& b = j.at("bounds");
        array(b, 4);
        require(b.size() == 4, "Bounds require four values");
        m.bounds = {number(b[0]), number(b[1]), number(b[2]), number(b[3])};
        m.tileOrigin = xy(j.at("tileOrigin"));
        m.provenance = enumeration<Provenance>(j.at("provenance"), 2);
        const auto& layers = j.at("layers");
        require(layers.is_object() && layers.size() <= 64, "Invalid layers");
        for (auto it = layers.begin(); it != layers.end(); ++it)
            m.layers[it.key()] = enumeration<LayerState>(it.value(), 3);
        array(j.at("features"), 100000);
        for (const auto& v : j.at("features"))
        {
            Feature f;
            f.layer = text(v.at("layer"));
            f.objectType = text(v.at("objectType"));
            f.sourceObjectId = text(v.at("sourceId"));
            f.fragmentId = text(v.at("fragmentId"));
            f.dataVersion = text(v.at("dataVersion"));
            f.provenance = enumeration<Provenance>(v.at("provenance"), 2);
            f.geometry = geometry(v.at("geometry"));
            const auto& a = v.at("properties");
            require(a.is_object() && a.size() <= 4096, "Invalid attributes");
            for (auto it = a.begin(); it != a.end(); ++it)
                f.properties[it.key()] = property(it.value());
            p.features.push_back(std::move(f));
        }
        if (!j.at("terrain").is_null())
        {
            const auto& v = j.at("terrain");
            TerrainData t;
            auto cols = integer(v.at("columns")), rows = integer(v.at("rows"));
            require(cols >= 2 && cols <= 1024 && rows >= 2 && rows <= 1024, "Invalid terrain dimensions");
            t.columns = static_cast<std::uint32_t>(cols);
            t.rows = static_cast<std::uint32_t>(rows);
            t.northWestSample = xy(v.at("northWestSample"));
            t.spacingMeters = number(v.at("spacing"));
            t.verticalDatum = text(v.at("verticalDatum"));
            t.provenance = enumeration<Provenance>(v.at("provenance"), 2);
            const auto& values = v.at("elevations");
            const auto& mask = v.at("valid");
            array(values);
            array(mask);
            const auto size = static_cast<std::size_t>(cols) * rows;
            require(values.size() == size && mask.size() == size, "Terrain dimensions/array mismatch");
            for (std::size_t i = 0; i < size; ++i)
            {
                auto flag = integer(mask[i]);
                require(flag == 0 || flag == 1, "Invalid validity flag");
                t.valid.push_back(static_cast<std::uint8_t>(flag));
                if (flag)
                {
                    auto z = number(values[i]);
                    require(std::abs(z) <= std::numeric_limits<float>::max(), "Elevation exceeds Float32");
                    t.elevations.push_back(static_cast<float>(z));
                }
                else
                {
                    require(values[i].is_null(), "Invalid elevation must encode as null");
                    t.elevations.push_back(std::numeric_limits<float>::quiet_NaN());
                }
            }
            if (v.contains("normals"))
            {
                array(v.at("normals"));
                require(v.at("normals").size() == size, "Terrain normals size mismatch");
                for (const auto& normal : v.at("normals"))
                {
                    array(normal, 3);
                    require(normal.size() == 3, "Normal needs XYZ");
                    std::array<float, 3> n;
                    for (int k = 0; k < 3; ++k)
                    {
                        double component = number(normal[k]);
                        require(std::abs(component) <= 1.001, "Normal component overflow");
                        n[k] = static_cast<float>(component);
                    }
                    t.normals.push_back(n);
                }
            }
            p.terrain = std::move(t);
        }
        if (j.contains("artifacts"))
        {
            array(j.at("artifacts"), 4);
            for (const auto& v : j.at("artifacts"))
            {
                RenderArtifact a;
                a.format = text(v.at("format"));
                a.path = text(v.at("path"));
                a.builderVersion = text(v.at("builderVersion"));
                a.contentHash = text(v.at("hash"));
                a.origin = xy(v.at("origin"));
                a.zScale = number(v.at("zScale"));
                auto bytes = integer(v.at("bytes"));
                require(bytes > 0, "Invalid artifact size");
                a.bytes = static_cast<std::uint64_t>(bytes);
                p.artifacts.push_back(std::move(a));
            }
        }
        if (auto e = validate(p))
            return *e;
        return p;
    }
    catch (const std::exception& e)
    {
        return TileError{ErrorCode::InvalidEncoding, e.what()};
    }
}

Result<std::string> encodeRequestJson(const TileRequest& r)
{
    if (r.schemaVersion != SchemaVersion)
        return TileError{ErrorCode::UnsupportedVersion, "Unsupported request schema"};
    auto layers = normalizeLayers(r.layers);
    if (auto e = std::get_if<TileError>(&layers))
        return *e;
    try
    {
        require(!r.tile.gridId.empty(), "Missing grid identity");
        boundedText(r.tile.gridId);
        boundedText(r.requestId);
        require(std::isfinite(r.terrainSpacingMeters) && r.terrainSpacingMeters > 0,
                "Invalid request spacing");
        J j = {{"schema", r.schemaVersion},
               {"requestId", r.requestId},
               {"tile", {{"gridId", r.tile.gridId}, {"x", r.tile.x}, {"y", r.tile.y}}},
               {"layers", std::get<std::vector<std::string>>(layers)},
               {"terrainSpacingMeters", r.terrainSpacingMeters}};
        return j.dump();
    }
    catch (const std::exception& e)
    {
        return TileError{ErrorCode::InvalidRequest, e.what()};
    }
}

Result<TileRequest> decodeRequestJson(const std::string& encoded)
{
    try
    {
        auto j = parseBounded(encoded);
        auto schema = integer(j.at("schema"));
        if (schema != SchemaVersion)
            return TileError{ErrorCode::UnsupportedVersion, "Unsupported request schema"};
        TileRequest r;
        r.requestId = text(j.at("requestId"));
        const auto& tile = j.at("tile");
        r.tile = {text(tile.at("gridId")), integer(tile.at("x")), integer(tile.at("y"))};
        array(j.at("layers"), 64);
        for (const auto& layer : j.at("layers"))
            r.layers.push_back(text(layer));
        r.terrainSpacingMeters = number(j.at("terrainSpacingMeters"));
        auto normalized = normalizeLayers(r.layers);
        if (auto e = std::get_if<TileError>(&normalized))
            return *e;
        r.layers = std::get<std::vector<std::string>>(normalized);
        auto valid = encodeRequestJson(r);
        if (auto e = std::get_if<TileError>(&valid))
            return *e;
        return r;
    }
    catch (const std::exception& e)
    {
        return TileError{ErrorCode::InvalidEncoding, e.what()};
    }
}
}
