#include <filesystem>
// 【阅读导航】动态场景装配入口：读取配置、连接 Provider、建立 3×3 TileManager，再交给 OSG Viewer 渲染。
#include "veylaxis/vgds/Service.hpp"
#include "veylaxis/vgds/OsgBuilder.hpp"
#include "veylaxis/streaming/TileManager.hpp"
#include "veylaxis/streaming/ModelRegistry.hpp"
#include "veylaxis/streaming/Motion.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "veylaxis/vgds/Configuration.hpp"
#include <nlohmann/json.hpp>
#include <ogr_spatialref.h>
#include <osg/NodeCallback>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osgGA/GUIEventHandler>
#include <osgGA/TrackballManipulator>
#include <osgViewer/View>
#include <osgViewer/Viewer>
#include <osg/observer_ptr>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <set>
#include <windows.h>
#include <psapi.h>

int showTerrain(osg::Node*, double, double, const OGRSpatialReference&, double, bool, const std::string&,
                osgGA::GUIEventHandler*);

namespace
{
using namespace veylaxis::vgds;
using namespace veylaxis::streaming;

struct MotionState
{
    VesselMotion motion;
    osg::observer_ptr<osgViewer::View> view;
};

class MotionEvents : public osgGA::GUIEventHandler
{
    std::shared_ptr<MotionState> state_;
    std::set<int> held_;

  public:
    explicit MotionEvents(std::shared_ptr<MotionState> state) : state_(std::move(state))
    {
    }

    bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter& action) override
    {
        if (auto* view = dynamic_cast<osgViewer::View*>(&action))
            state_->view = view;
        auto& m = state_->motion;
        bool down = event.getEventType() == osgGA::GUIEventAdapter::KEYDOWN,
             up = event.getEventType() == osgGA::GUIEventAdapter::KEYUP;
        if (!down && !up)
            return false;
        if (down && !held_.insert(event.getKey()).second)
            return false;
        if (up)
            held_.erase(event.getKey());
        switch (event.getKey())
        {
        case osgGA::GUIEventAdapter::KEY_Left:
            m.automatic = false;
            m.east = down ? -1 : 0;
            return true;
        case osgGA::GUIEventAdapter::KEY_Right:
            m.automatic = false;
            m.east = down ? 1 : 0;
            return true;
        case osgGA::GUIEventAdapter::KEY_Up:
            m.automatic = false;
            m.north = down ? 1 : 0;
            return true;
        case osgGA::GUIEventAdapter::KEY_Down:
            m.automatic = false;
            m.north = down ? -1 : 0;
            return true;
        case 'p':
        case 'P':
            if (down)
                m.paused = !m.paused;
            return true;
        case 'f':
        case 'F':
            if (down)
                m.follow = !m.follow;
            return true;
        default:
            return false;
        }
    }
};

// 每帧更新顺序：推进船位 → 更新船模型与跟随相机 → tick 请求/卸载区块。
struct StreamingUpdate final : osg::NodeCallback
{
    std::shared_ptr<TileManager> manager;
    std::shared_ptr<ITileProvider> service;
    std::shared_ptr<MotionState> motion;
    osg::ref_ptr<osg::MatrixTransform> vessel;
    XY origin;
    double lastTime = -1, lastReport = -1, firstTime = -1, duration{};
    std::vector<double> frameTimes;
    std::uint64_t frames{};

    StreamingUpdate(std::shared_ptr<TileManager> m, std::shared_ptr<ITileProvider> s,
                    std::shared_ptr<MotionState> state, osg::MatrixTransform* boat, XY o)
        : manager(std::move(m)), service(std::move(s)), motion(std::move(state)), vessel(boat), origin(o)
    {
    }

    void operator()(osg::Node* node, osg::NodeVisitor* nv) override
    {
        double time = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0,
               dt = lastTime < 0 ? 0 : std::max(0.0, time - lastTime);
        if (firstTime < 0)
            firstTime = time;
        lastTime = time;
        ++frames;
        if (frames > 30 && frameTimes.size() < 4096)
            frameTimes.push_back(dt * 1000);
        auto before = motion->motion.position;
        motion->motion.advance(std::min(dt, .25));
        XY position = motion->motion.position;
        vessel->setMatrix(osg::Matrixd::translate(position.x - origin.x, position.y - origin.y, 0));
        if (motion->motion.follow && motion->view.valid())
            if (auto* manipulator =
                    dynamic_cast<osgGA::TrackballManipulator*>(motion->view->getCameraManipulator()))
                manipulator->setCenter(manipulator->getCenter() +
                                       osg::Vec3d(position.x - before.x, position.y - before.y, 0));
        if (duration > 0 && time - firstTime >= duration && motion->view.valid())
            if (auto* viewer = dynamic_cast<osgViewer::Viewer*>(motion->view.get()))
                viewer->setDone(true);
        if (auto e = manager->tick(position))
            std::cerr << "Tile update: " << e->message << '\n';
        if (time - lastReport > 1)
        {
            auto s = manager->stats();
            std::uint64_t hits = 0, resident = 0;
            if (auto local = std::dynamic_pointer_cast<LocalService>(service))
                hits = local->stats().hits;
            if (auto catalog = std::dynamic_pointer_cast<CatalogProvider>(service))
            {
                auto c = catalog->stats();
                hits = c.cacheHits;
                resident = c.residentCells;
            }
            if (motion->view.valid())
                if (auto* window = dynamic_cast<osgViewer::GraphicsWindow*>(
                        motion->view->getCamera()->getGraphicsContext()))
                {
                    std::ostringstream title;
                    title << "Veylaxis | ENC Catalog | active " << s.active << " loading " << s.loading
                          << " | "
                          << (motion->motion.paused      ? "paused"
                              : motion->motion.automatic ? "automatic"
                                                         : "manual")
                          << " | XYZ 1:1";
                    window->setWindowName(title.str());
                }
            std::cout << "VGDS XY=" << position.x << "," << position.y << " active=" << s.active
                      << " loading=" << s.loading << " failed=" << s.failed << " requests=" << s.requests
                      << " cache_hits=" << hits << " resident_ENC=" << resident << " unloaded=" << s.unloaded
                      << " discarded=" << s.discarded << " mesh_CPU_bytes=" << s.activeMeshBytes
                      << " pending_mesh_bytes=" << s.pendingMeshBytes
                      << " last_load_ms=" << s.lastLoadMilliseconds << '\n'
                      << std::flush;
            lastReport = time;
        }
        traverse(node, nv);
    }
};
}
// 阅读主链路从这里开始：配置装配完毕后，通过同一个 Provider 接口连接真实或模拟数据。
int runVgdsViewer(int argc, char** argv)
try
{
    std::string path = "config/vgds-catalog.json",
                modelConfiguration = std::filesystem::exists("config/navigation_models.local.json")
                                         ? "config/navigation_models.local.json"
                                         : "config/navigation_models.json",
                smoke, reportPath;
    double duration = 0;
    bool marine = true, tour = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--tour")
            tour = true;
        else if (i + 1 >= argc)
            throw std::runtime_error("Missing VGDS option value");
        else if (arg == "--config")
            path = argv[++i];
        else if (arg == "--navigation-models")
            modelConfiguration = argv[++i];
        else if (arg == "--smoke-test")
            smoke = argv[++i];
        else if (arg == "--duration")
            duration = std::stod(argv[++i]);
        else if (arg == "--report")
            reportPath = argv[++i];
        else if (arg == "--marine")
        {
            std::string mode = argv[++i];
            if (mode != "on" && mode != "off")
                throw std::runtime_error("--marine on|off");
            marine = mode == "on";
        }
        else
            throw std::runtime_error("Unknown VGDS option");
    }
    auto definition = readServiceConfiguration(path);
    if (auto e = std::get_if<TileError>(&definition))
        throw std::runtime_error(e->message);
    auto setup = std::get<ServiceSetup>(std::move(definition));
    const auto grid = setup.dataset.grid;
    auto options = setup.service;
    options.builder = buildOsgb;
    options.builderVersion = OsgBuilderVersion;
    if (setup.islands)
    {
        options.builder = proceduralBuilder(setup.islands);
        options.builderVersion = setup.islands->version();
    }
    const auto start = setup.start;
    if (setup.source)
        std::cout << "S57 LNDARE=" << setup.source->landPolygons << " points=" << setup.source->landPoints
                  << " lines=" << setup.source->landLines << " COALNE=" << setup.source->coastLines
                  << " unsupported=" << setup.source->unsupportedGeometries
                  << " update=" << setup.source->update << " startup_ms=" << setup.source->loadMilliseconds
                  << '\n';
    if (setup.testArea)
        std::cout << "Real land test: " << setup.testArea->sourceObjectId
                  << " tile=" << setup.testArea->centerTile.x << "," << setup.testArea->centerTile.y << '\n';
    auto service = makeProvider(setup, options);
    if (setup.catalog)
    {
        auto stats = setup.catalog->stats();
        std::cout << "ENC Catalog cells=" << stats.files << " indexed=" << stats.indexed
                  << " reused=" << stats.reused << '\n';
    }
    // 这里是进程内调用，但仍走 JSON 编解码，以保持客户端与服务数据契约一致。
    auto client = std::make_shared<TileClient>(
        [service](const std::string& request)
        {
            return requestProviderJson(*service, request);
        });
    auto centered = locate(grid, start);
    if (auto e = std::get_if<TileError>(&centered))
        throw std::runtime_error(e->message);
    auto extent = std::get<Bounds>(boundsOf(grid, std::get<TileID>(centered)));
    XY sceneOrigin{extent.minX, extent.minY};
    osg::ref_ptr<osg::Group> root = new osg::Group, tileRoot = new osg::Group;
    root->setName("VGDS 3x3 dynamic land");
    // 场景再次减去附近的投影原点，避免把数百万米大坐标直接交给渲染。
    osg::ref_ptr<osg::MatrixTransform> recenter = new osg::MatrixTransform;
    recenter->setMatrix(
        osg::Matrixd::translate(grid.origin.x - sceneOrigin.x, grid.origin.y - sceneOrigin.y, 0));
    recenter->addChild(tileRoot);
    root->addChild(recenter);
    auto models = std::make_shared<ModelRegistry>(modelConfiguration);
    ManagerConfig managerConfig;
    managerConfig.layers = setup.layers;
    managerConfig.terrainSpacingMeters = setup.terrainSpacingMeters;
    if (auto catalog = std::dynamic_pointer_cast<CatalogProvider>(service))
        managerConfig.cancelRequest = [catalog](const std::string& id)
        {
            catalog->cancel(id);
        };
    auto manager = std::make_shared<TileManager>(grid, tileRoot, makeLoader(client, setup.islands, models),
                                                 managerConfig);
    // 首屏允许等待九块就绪；进入 Viewer 后，每帧 tick 只消费完成结果，不阻塞等待生成。
    // Initial staging before opening the window. Rendering never waits on requests.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;)
    {
        if (auto e = manager->tick(start))
            throw std::runtime_error(e->message);
        auto s = manager->stats();
        if (s.active == 9)
            break;
        if (s.failed && s.loading == 0)
            throw std::runtime_error("Initial tile loads failed");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Initial tiles timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto state = std::make_shared<MotionState>();
    state->motion.start = state->motion.position = start;
    state->motion.speed = setup.motionSpeedMetersPerSecond;
    state->motion.tileSize = grid.tileSizeMeters;
    state->motion.automatic = tour;
    osg::ref_ptr<osg::MatrixTransform> vessel = new osg::MatrixTransform;
    nlohmann::json modelSettings;
    {
        std::ifstream input(modelConfiguration);
        if (input)
            input >> modelSettings;
    }
    auto ship = models->loadModel(modelSettings.value("vesselModelPath", std::string{}));
    if (ship)
        vessel->addChild(ship);
    else
        std::cerr << "Vessel model unavailable; motion continues without a placeholder\n";
    root->addChild(vessel);
    vessel->setMatrix(osg::Matrixd::translate(start.x - sceneOrigin.x, start.y - sceneOrigin.y, 0));
    osg::ref_ptr<StreamingUpdate> update = new StreamingUpdate(manager, service, state, vessel, sceneOrigin);
    root->setUpdateCallback(update);
    std::cout
        << "Motion: arrows move vessel; P pause; F camera follow; --tour automatic continuous movement.\n";
    update->duration = duration;
    OGRSpatialReference srs;
    if (srs.SetFromUserInput(grid.horizontalCrs.c_str()) != OGRERR_NONE)
        throw std::runtime_error("Invalid viewer CRS");
    int result =
        showTerrain(root, sceneOrigin.x, sceneOrigin.y, srs, 1, marine, smoke, new MotionEvents(state));
    if (reportPath.empty() && !smoke.empty())
        reportPath = smoke + ".json";
    if (!reportPath.empty())
    {
        auto stats = manager->stats();
        auto times = update->frameTimes;
        std::sort(times.begin(), times.end());
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory));
        nlohmann::json report = {{"frames", update->frames},
                                 {"requests", stats.requests},
                                 {"loaded", stats.loaded},
                                 {"unloaded", stats.unloaded},
                                 {"failures", stats.failures},
                                 {"cancellations", stats.cancellations},
                                 {"mesh_bytes", stats.activeMeshBytes},
                                 {"working_set_bytes", memory.WorkingSetSize},
                                 {"peak_working_set_bytes", memory.PeakWorkingSetSize},
                                 {"private_bytes", memory.PrivateUsage},
                                 {"position", {state->motion.position.x, state->motion.position.y}}};
        report["frame_p95_ms"] =
            times.empty() ? nlohmann::json(nullptr)
                          : nlohmann::json(times[static_cast<std::size_t>((times.size() - 1) * .95)]);
        std::ofstream output(reportPath);
        output << report.dump(2);
    }
    root->setUpdateCallback(nullptr);
    root->setEventCallback(nullptr);
    manager->clear();
    return result;
}
catch (const std::exception& e)
{
    std::cerr << "VGDS viewer: " << e.what() << '\n';
    return 1;
}
