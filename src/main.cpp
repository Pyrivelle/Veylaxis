// 【阅读导航】程序总入口：无参数进入 VGDS 动态场景；reconstruct 和 view 保留离线海底工具链。
#include <iostream>
#include <string>

int runReconstruction(int argc, char** argv);
int runViewer(int argc, char** argv);
#ifdef VEYLAXIS_HAS_OSGB
int runVgdsViewer(int argc, char** argv);
#endif

int main(int argc, char** argv)
{
    // F5 opens the moving catalog-backed scene. Reconstruction remains explicit.
    if (argc == 1)
    {
#ifdef VEYLAXIS_HAS_OSGB
        char mode[] = "vgds", option[] = "--config", config[] = "config/vgds-catalog.json";
        char* arguments[] = {mode, option, config};
        return runVgdsViewer(3, arguments);
#else
        char defaultMode[] = "view", defaultRaster[] = "output/region001/region001_elevation.tif";
        char* viewerArguments[] = {defaultMode, defaultRaster};
        return runViewer(2, viewerArguments);
#endif
    }
    const std::string command = argv[1];
    if (command == "reconstruct")
        return runReconstruction(argc - 1, argv + 1);
    if (command == "view")
        return runViewer(argc - 1, argv + 1);
    if (command == "vgds")
    {
#ifdef VEYLAXIS_HAS_OSGB
        return runVgdsViewer(argc - 1, argv + 1);
#else
        std::cerr << "VGDS viewer requires VEYLAXIS_VGDS_OSGB=ON\n";
        return 1;
#endif
    }
    if (command == "--help" || command == "-h")
    {
        std::cout << "Veylaxis reconstruct [options]  Reconstruct S57 seabed (default: region001)\n"
                  << "Veylaxis reconstruct --help    Reconstruction options\n"
                  << "Veylaxis view elevation.tif [--export terrain.osgt] [--z-scale factor]\n"
                  << "Veylaxis vgds --config config/vgds-mock.json [--tour] [--marine on|off]\n"
                  << "Viewer default: SilverLining sky + Triton ocean + seabed; XYZ metres, Z x1.\n";
        return 0;
    }
    std::cerr << "Unknown command: " << command << ". Use Veylaxis --help.\n";
    return 1;
}
