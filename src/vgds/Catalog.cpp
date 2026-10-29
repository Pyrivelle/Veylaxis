// 【阅读导航】多海图选源入口：索引真实覆盖范围、确定优先源、按 Tile 合并裁剪结果并维护缓存。
#include "veylaxis/vgds/Catalog.hpp"
#include "veylaxis/vgds/S57.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "TileCache.hpp"
#include "PreparedShape.hpp"
#include <ogrsf_frmts.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <set>
#include <atomic>
#include <deque>
#include <chrono>

namespace veylaxis::vgds
{
namespace
{
using J = nlohmann::json;
namespace fs = std::filesystem;
using G = std::unique_ptr<OGRGeometry>;

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}

G rectangle(const Bounds& b)
{
    OGRLinearRing r;
    r.addPoint(b.minX, b.minY);
    r.addPoint(b.maxX, b.minY);
    r.addPoint(b.maxX, b.maxY);
    r.addPoint(b.minX, b.maxY);
    r.closeRings();
    auto p = std::make_unique<OGRPolygon>();
    p->addRing(&r);
    return p;
}

Ring ring(const OGRLinearRing& r)
{
    Ring out;
    for (int i = 0; i < r.getNumPoints() - 1; ++i)
        out.points.push_back({r.getX(i), r.getY(i)});
    out.edges.assign(out.points.size(), BoundaryKind::ArtificialClip);
    return out;
}

void polygons(const OGRGeometry& g, MultiPolygon& out)
{
    if (wkbFlatten(g.getGeometryType()) == wkbPolygon)
    {
        auto* p = g.toPolygon();
        if (p->get_Area() < 1e-8)
            return;
        Polygon v;
        v.outer = ring(*p->getExteriorRing());
        for (int i = 0; i < p->getNumInteriorRings(); ++i)
            v.holes.push_back(ring(*p->getInteriorRing(i)));
        out.polygons.push_back(std::move(v));
    }
    else if (OGR_GT_IsSubClassOf(wkbFlatten(g.getGeometryType()), wkbGeometryCollection))
        for (const auto* child : *g.toGeometryCollection())
            polygons(*child, out);
}

std::string manifest(const fs::path& base)
{
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(base.parent_path()))
    {
        auto p = e.path();
        auto ext = p.extension().string();
        if (p.stem() == base.stem() && ext.size() == 4 &&
            std::all_of(ext.begin() + 1, ext.end(),
                        [](char c)
                        {
                            return c >= '0' && c <= '9';
                        }))
            files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    J j = J::array();
    for (const auto& p : files)
        j.push_back({p.filename().string(), fileHash(p.string())});
    return contentHash(j.dump() + GDALVersionInfo("RELEASE_NAME"));
}

bool intersects(const Bounds& a, const Bounds& b)
{
    return a.maxX >= b.minX && a.minX <= b.maxX && a.maxY >= b.minY && a.minY <= b.maxY;
}
}

struct EncCatalog::Impl
{
    GridSpec grid;
    CatalogConfig config;
    std::vector<CatalogEntry> entries;
    std::vector<G> coverage;
    std::vector<std::unique_ptr<PreparedShape>> prepared;
    CatalogStats stats;
    mutable std::recursive_mutex geometryMutex;
};

// 启动阶段建立或复用海图覆盖索引；后续 Tile 请求只加载涉及的完整源数据集。
EncCatalog::EncCatalog(GridSpec grid, CatalogConfig config) : impl_(std::make_unique<Impl>())
{
    if (auto e = validate(grid))
        throw std::invalid_argument(e->message);
    if (config.root.empty() || config.indexFile.empty() || config.maxResidentCells < 1)
        throw std::invalid_argument("Invalid catalog configuration");
    auto& i = *impl_;
    i.grid = std::move(grid);
    i.config = std::move(config);
    GDALAllRegister();
    OGRSpatialReference target;
    if (target.SetFromUserInput(i.grid.horizontalCrs.c_str()) != OGRERR_NONE || !target.IsProjected() ||
        std::abs(target.GetLinearUnits() - 1) > 1e-9)
        throw std::runtime_error("Catalog requires metre projected CRS");
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    J previous = J::object();
    try
    {
        std::ifstream input(i.config.indexFile);
        if (input)
            input >> previous;
    }
    catch (...)
    {
        previous = J::object();
    }
    J next = {{"format", 1}, {"crs", i.grid.horizontalCrs}, {"cells", J::object()}};
    std::vector<fs::path> bases;
    for (const auto& entry : fs::recursive_directory_iterator(i.config.root))
        if (entry.is_regular_file() && entry.path().extension() == ".000")
            bases.push_back(fs::absolute(entry.path()));
    std::sort(bases.begin(), bases.end());
    if (bases.empty())
        throw std::runtime_error("Catalog has no .000 base files");

    struct Indexed
    {
        CatalogEntry entry;
        G geometry;
    };

    std::vector<Indexed> indexed;
    for (const auto& base : bases)
    {
        CatalogEntry e;
        e.path = base.string();
        e.cell = base.stem().string();
        e.version = manifest(base);
        G coverage;
        J record;
        bool reused = false;
        try
        {
            record = previous.at("cells").at(e.path);
            if (previous.at("format") != 1 || previous.at("crs") != i.grid.horizontalCrs ||
                record.at("version") != e.version)
                throw std::runtime_error("Stale index");
            auto wkt = record.at("coverage").get<std::string>();
            OGRGeometry* raw = nullptr;
            if (OGRGeometryFactory::createFromWkt(wkt.c_str(), nullptr, &raw) != OGRERR_NONE || !raw)
                throw std::runtime_error("Invalid indexed coverage");
            coverage.reset(raw);
            reused = true;
        }
        catch (...)
        {
            coverage.reset();
        }
        if (!reused)
        {
            const char* drivers[] = {"S57", nullptr};
            const char* options[] = {"UPDATES=APPLY", nullptr};
            std::unique_ptr<GDALDataset, decltype(&GDALClose)> source(
                static_cast<GDALDataset*>(
                    GDALOpenEx(e.path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, drivers, options, nullptr)),
                GDALClose);
            if (!source)
                throw std::runtime_error("Catalog cannot open " + e.path);
            auto* idLayer = source->GetLayerByName("DSID");
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> id(
                idLayer ? idLayer->GetNextFeature() : nullptr, OGRFeature::DestroyFeature);
            if (!id)
                throw std::runtime_error("Missing DSID in " + e.cell);
            e.compilationScale = id->GetFieldAsDouble("DSPM_CSCL");
            e.edition = id->GetFieldAsString("DSID_EDTN");
            e.update = id->GetFieldAsString("DSID_UPDN");
            if (e.compilationScale <= 0)
                throw std::runtime_error("Invalid compilation scale");
            auto* layer = source->GetLayerByName("M_COVR");
            if (!layer || !layer->GetSpatialRef())
                throw std::runtime_error("Missing M_COVR in " + e.cell);
            auto srs = *layer->GetSpatialRef();
            srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)>
                transform(OGRCreateCoordinateTransformation(&srs, &target),
                          OCTDestroyCoordinateTransformation);
            if (!transform)
                throw std::runtime_error("Coverage projection failed");
            G excluded;
            while (auto* raw = layer->GetNextFeature())
            {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                    raw, OGRFeature::DestroyFeature);
                auto* original = feature->GetGeometryRef();
                if (!original || original->IsEmpty())
                    continue;
                G geometry(original->clone());
                if (geometry->transform(transform.get()) != OGRERR_NONE)
                    throw std::runtime_error("Coverage transformation failed");
                auto& output = feature->GetFieldAsInteger("CATCOV") == 1 ? coverage : excluded;
                if (!output)
                    output = std::move(geometry);
                else
                {
                    G merged(output->Union(geometry.get()));
                    if (!merged)
                        throw std::runtime_error("Coverage union failed");
                    output = std::move(merged);
                }
            }
            if (!coverage)
                throw std::runtime_error("No CATCOV=1 coverage in " + e.cell);
            if (excluded)
            {
                G cut(coverage->Difference(excluded.get()));
                if (!cut)
                    throw std::runtime_error("Coverage exclusion failed");
                coverage = std::move(cut);
            }
            if (!coverage->IsValid())
                throw std::runtime_error("Invalid M_COVR in " + e.cell);
            char* wkt = nullptr;
            coverage->exportToWkt(&wkt);
            record = {{"version", e.version},
                      {"scale", e.compilationScale},
                      {"edition", e.edition},
                      {"update", e.update},
                      {"coverage", wkt ? wkt : ""}};
            CPLFree(wkt);
            ++i.stats.indexed;
        }
        else
        {
            e.compilationScale = record.at("scale");
            e.edition = record.at("edition");
            e.update = record.at("update");
            ++i.stats.reused;
        }
        OGREnvelope bounds;
        coverage->getEnvelope(&bounds);
        e.bounds = {bounds.MinX, bounds.MinY, bounds.MaxX, bounds.MaxY};
        next["cells"][e.path] = record;
        indexed.push_back({std::move(e), std::move(coverage)});
        ++i.stats.files;
    }
    std::sort(indexed.begin(), indexed.end(),
              [](const auto& a, const auto& b)
              {
                  if (a.entry.compilationScale != b.entry.compilationScale)
                      return a.entry.compilationScale < b.entry.compilationScale;
                  return a.entry.cell < b.entry.cell;
              });
    for (auto& entry : indexed)
    {
        i.entries.push_back(std::move(entry.entry));
        i.prepared.push_back(std::make_unique<PreparedShape>(*entry.geometry));
        i.coverage.push_back(std::move(entry.geometry));
    }
    fs::path path(i.config.indexFile);
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << next.dump(2);
    if (!output)
        throw std::runtime_error("Cannot persist ENC catalog");
}

EncCatalog::~EncCatalog() = default;

const GridSpec& EncCatalog::grid() const
{
    return impl_->grid;
}

const CatalogConfig& EncCatalog::config() const
{
    return impl_->config;
}

const std::vector<CatalogEntry>& EncCatalog::entries() const
{
    return impl_->entries;
}

CatalogStats EncCatalog::stats() const
{
    return impl_->stats;
}

std::vector<std::size_t> EncCatalog::query(const Bounds& bounds) const
{
    auto box = rectangle(bounds);
    std::vector<std::size_t> out;
    for (std::size_t n = 0; n < impl_->entries.size(); ++n)
        if (intersects(bounds, impl_->entries[n].bounds) && impl_->coverage[n]->Intersects(box.get()))
            out.push_back(n);
    return out;
}

std::optional<std::size_t> EncCatalog::owner(XY p) const
{
    std::lock_guard<std::recursive_mutex> guard(impl_->geometryMutex);
    for (std::size_t n = 0; n < impl_->entries.size(); ++n)
    {
        const auto& b = impl_->entries[n].bounds;
        if (p.x >= b.minX && p.x <= b.maxX && p.y >= b.minY && p.y <= b.maxY &&
            impl_->prepared[n]->intersects(p))
            return n;
    }
    return {};
}

// 从当前源的覆盖范围扣除更高优先级覆盖，避免重叠海图重复供数。
std::optional<Geometry> EncCatalog::ownedRegion(std::size_t index, const Bounds& bounds) const
{
    auto box = rectangle(bounds);
    G owned(impl_->coverage.at(index)->Intersection(box.get()));
    if (!owned)
        throw std::runtime_error("Coverage intersection failed");
    for (auto other : query(bounds))
    {
        if (other >= index)
            break;
        G remainder(owned->Difference(impl_->coverage[other].get()));
        if (!remainder)
            throw std::runtime_error("Coverage priority subtraction failed");
        owned = std::move(remainder);
        if (owned->IsEmpty())
            return {};
    }
    MultiPolygon polygonsOut;
    polygons(*owned, polygonsOut);
    if (polygonsOut.polygons.empty())
        return {};
    if (polygonsOut.polygons.size() == 1)
        return polygonsOut.polygons.front();
    return polygonsOut;
}

struct CatalogProvider::Impl
{
    std::shared_ptr<EncCatalog> catalog;
    ServiceConfig options;
    std::unique_ptr<TileCache> cache;
    mutable std::mutex mutex;
    std::uint64_t clock{}, loads{}, evictions{};
    mutable std::mutex publishedMutex;
    mutable CatalogStats published;
    std::mutex cancellationMutex;
    std::map<std::string, std::shared_ptr<std::atomic_bool>> active;
    std::set<std::string> cancelled;
    std::deque<std::string> cancelledOrder;

    struct Cell
    {
        std::shared_ptr<Dataset> dataset;
        std::uint64_t touched;
    };

    std::map<std::size_t, Cell> cells;

    std::shared_ptr<Dataset> cell(std::size_t index)
    {
        if (auto found = cells.find(index); found != cells.end())
        {
            found->second.touched = ++clock;
            return found->second.dataset;
        }
        requestCheckpoint();
        const auto& entry = catalog->entries().at(index);
        auto dataset = std::make_shared<Dataset>(
            value(readS57(entry.path, catalog->grid(), nullptr,
                          {catalog->config().bathymetry, catalog->config().maritime})));
        requestCheckpoint();
        ++loads;
        if (options.sourceLoaded)
            options.sourceLoaded(*dataset);
        cells[index] = {dataset, ++clock};
        while (cells.size() > catalog->config().maxResidentCells)
        {
            auto oldest = std::min_element(cells.begin(), cells.end(),
                                           [](const auto& a, const auto& b)
                                           {
                                               return a.second.touched < b.second.touched;
                                           });
            if (options.sourceEvicted)
                options.sourceEvicted(oldest->second.dataset->id);
            cells.erase(oldest);
            ++evictions;
        }
        return dataset;
    }
};

CatalogProvider::CatalogProvider(std::shared_ptr<EncCatalog> catalog, ServiceConfig options)
    : impl_(std::make_unique<Impl>())
{
    if (!catalog || options.cacheDirectory.empty() || (options.osgbEnabled && !options.builder))
        throw std::invalid_argument("Invalid catalog provider");
    impl_->catalog = std::move(catalog);
    impl_->options = std::move(options);
    const auto& c = impl_->catalog->config();
    impl_->cache =
        std::make_unique<TileCache>(impl_->options.cacheDirectory, c.maxCacheEntries, c.maxCacheBytes);
}

CatalogProvider::~CatalogProvider() = default;

void CatalogProvider::cancel(const std::string& id)
{
    auto& i = *impl_;
    std::lock_guard<std::mutex> guard(i.cancellationMutex);
    auto active = i.active.find(id);
    if (active != i.active.end())
    {
        *active->second = true;
        return;
    }
    if (i.cancelled.insert(id).second)
        i.cancelledOrder.push_back(id);
    while (i.cancelledOrder.size() > 64)
    {
        i.cancelled.erase(i.cancelledOrder.front());
        i.cancelledOrder.pop_front();
    }
}

const EncCatalog& CatalogProvider::catalog() const
{
    return *impl_->catalog;
}

CatalogStats CatalogProvider::stats() const
{
    std::unique_lock<std::mutex> guard(impl_->mutex, std::try_to_lock);
    std::lock_guard<std::mutex> published(impl_->publishedMutex);
    if (guard.owns_lock())
    {
        auto s = impl_->catalog->stats();
        s.cellLoads = impl_->loads;
        s.cellEvictions = impl_->evictions;
        s.residentCells = impl_->cells.size();
        s.cacheHits = impl_->cache->hits;
        s.generations = impl_->cache->generations;
        s.diskEvictions = impl_->cache->evictions;
        impl_->published = s;
    }
    return impl_->published;
}
// 真实多源请求主入口：选源、版本缓存键、裁剪、海底采样以及可选渲染缓存。
TileResult CatalogProvider::request(const TileRequest& request)
try
{
    auto encoded = encodeRequestJson(request);
    if (auto e = std::get_if<TileError>(&encoded))
        return *e;
    auto r = value(decodeRequestJson(std::get<std::string>(encoded)));
    auto& i = *impl_;
    auto token = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(i.cancellationMutex);
        if (i.active.count(r.requestId))
            return TileError{ErrorCode::InvalidRequest, "Duplicate active request ID"};
        if (i.cancelled.erase(r.requestId))
            *token = true;
        i.active[r.requestId] = token;
    }

    struct Registration
    {
        Impl& impl;
        std::string id;

        ~Registration()
        {
            std::lock_guard<std::mutex> lock(impl.cancellationMutex);
            impl.active.erase(id);
        }
    } registration{i, r.requestId};

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(i.catalog->config().requestTimeoutMilliseconds);
    RequestScope scope(
        [token, deadline]
        {
            return token->load() || std::chrono::steady_clock::now() > deadline;
        });
    requestCheckpoint();
    auto b = value(boundsOf(i.catalog->grid(), r.tile));
    auto selected = i.catalog->query(b);
    std::lock_guard<std::mutex> guard(i.mutex);
    requestCheckpoint();
    r.requestId.clear();
    J versions = J::array();
    for (auto index : selected)
        versions.push_back({i.catalog->entries()[index].cell, i.catalog->entries()[index].version});
    const auto version = contentHash(versions.dump());
    const auto& grid = i.catalog->grid();
    auto key = contentHash(
        J{{"request", value(encodeRequestJson(r))},
          {"versions", versions},
          {"grid", {grid.id, grid.horizontalCrs, grid.origin.x, grid.origin.y, grid.tileSizeMeters}},
          {"algorithm", "catalog-priority-v3-prepared"},
          {"navigation_adapter", i.catalog->config().maritime ? "s57-maritime-v3-model-classes" : "disabled"},
          {"bathy", i.catalog->config().bathymetry},
          {"maritime", i.catalog->config().maritime},
          {"osgb", i.options.osgbEnabled},
          {"builder", i.options.builderVersion}}
            .dump());
    if (!i.options.osgbEnabled && i.options.sourceLoaded)
        for (auto index : selected)
            i.cell(index);
    return i.cache->fetch(
        key, request,
        [&](const std::string& path) -> TileResult
        {
            TilePackage p;
            auto& m = p.metadata;
            m.datasetId = "enc-catalog";
            m.dataVersion = version;
            m.algorithmVersion = "catalog-priority-v2";
            m.grid = grid;
            m.tile = r.tile;
            m.bounds = b;
            m.tileOrigin = {b.minX, b.minY};
            m.provenance = Provenance::Observed;
            for (const auto& layer : r.layers)
            {
                bool supported =
                    layer == "land" || layer == "coastline" ||
                    (i.catalog->config().bathymetry && layer == "terrain") ||
                    (i.catalog->config().maritime &&
                     (layer == "fairway" || layer == "dredged_areas" || layer == "navigation_aids"));
                m.layers[layer] = supported ? (selected.empty() ? LayerState::NoCoverage : LayerState::Empty)
                                            : LayerState::Unsupported;
            }
            // 每个源先限定有效覆盖；点还要检查唯一 owner，避免公共边及重叠源重复返回。
            for (auto index : selected)
            {
                auto region = i.catalog->ownedRegion(index, b);
                if (!region)
                    continue;
                auto data = i.cell(index);
                for (const auto& f : data->features)
                {
                    requestCheckpoint();
                    if (!m.layers.count(f.layer))
                        continue;
                    if (!intersects(geometryBounds(f.geometry), b))
                        continue;
                    auto pieces = value(intersectFeature(f, *region, b, r.tile));
                    for (auto& piece : pieces)
                    {
                        if (auto point = std::get_if<Point>(&piece.geometry))
                        {
                            auto owner = i.catalog->owner(point->position);
                            if (!owner || *owner != index)
                                continue;
                        }
                        m.layers[piece.layer] = LayerState::Available;
                        p.features.push_back(std::move(piece));
                    }
                }
            }
            if (m.layers.count("terrain") && i.catalog->config().bathymetry)
            {
                std::map<std::size_t, std::shared_ptr<Dataset>> samplingCells;
                std::set<std::string> datums;
                TerrainSource source;
                source.verticalDatum = "unknown: no ENC sounding coverage";
                source.algorithmVersion = "catalog-adjacent-contour-v1";
                source.elevation = [&](XY position) -> std::optional<double>
                {
                    auto owner = i.catalog->owner(position);
                    if (!owner)
                        return {};
                    auto& data = samplingCells[*owner];
                    if (!data)
                        data = i.cell(*owner);
                    if (!data->terrainSource)
                        return {};
                    auto z = data->terrainSource->elevation(position);
                    if (z)
                        datums.insert(data->terrainSource->verticalDatum);
                    return z;
                };
                auto sampled = sampleTerrain(grid, r.tile, r.terrainSpacingMeters, source);
                if (auto error = std::get_if<TileError>(&sampled))
                    return *error;
                if (datums.size() > 1)
                    return TileError{ErrorCode::GenerationFailure,
                                     "Mixed ENC vertical datums require explicit conversion"};
                p.terrain = std::get<TerrainData>(std::move(sampled));
                if (!datums.empty())
                    p.terrain->verticalDatum = *datums.begin();
                m.layers["terrain"] =
                    std::find(p.terrain->valid.begin(), p.terrain->valid.end(), 1) != p.terrain->valid.end()
                        ? LayerState::Available
                        : LayerState::NoCoverage;
                m.algorithmVersion += "+" + source.algorithmVersion;
            }
            if (auto error = validate(p))
                return *error;
            if (i.options.osgbEnabled)
                p.artifacts.push_back(i.options.builder(p, path));
            return p;
        });
}
catch (const RequestCancelled& e)
{
    return TileError{ErrorCode::Cancelled, e.what()};
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::GenerationFailure, e.what()};
}
}
