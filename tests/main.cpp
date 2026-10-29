#include <iostream>
#include <string>

int runReconstructionTests();
int runRasterCheck(int argc, char** argv);
int runSceneCheck(int argc, char** argv);
int runCameraCheck();
int runTileScene(int argc, char** argv);
int runVgdsTests();
int runVgdsServiceTests();
int runVgdsS57Tests();
int runCatalogTests();
int runIslandTerrainTests();
int runSeabedMaritimeTests();
int runAcceptanceTests();
#ifdef VEYLAXIS_HAS_OSGB
int runVgdsStreamingTests();
int runNavigationModelTests();
int runDmuModelTests();
#endif

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        const int result = runReconstructionTests();
        return result ? result : runCameraCheck();
    }
    if (std::string(argv[1]) == "--raster")
        return runRasterCheck(argc - 1, argv + 1);
    if (std::string(argv[1]) == "--scene")
        return runSceneCheck(argc - 1, argv + 1);
    if (std::string(argv[1]) == "--tile-scene")
        return runTileScene(argc - 1, argv + 1);
    if (std::string(argv[1]) == "--vgds")
        return runVgdsTests();
    if (std::string(argv[1]) == "--vgds-service")
        return runVgdsServiceTests();
    if (std::string(argv[1]) == "--vgds-s57")
        return runVgdsS57Tests();
    if (std::string(argv[1]) == "--catalog")
        return runCatalogTests();
    if (std::string(argv[1]) == "--island")
        return runIslandTerrainTests();
    if (std::string(argv[1]) == "--seabed-maritime")
        return runSeabedMaritimeTests();
    if (std::string(argv[1]) == "--acceptance")
        return runAcceptanceTests();
#ifdef VEYLAXIS_HAS_OSGB
    if (std::string(argv[1]) == "--vgds-streaming")
        return runVgdsStreamingTests();
    if (std::string(argv[1]) == "--navigation-models")
        return runNavigationModelTests();
    if (std::string(argv[1]) == "--dmu-models")
        return runDmuModelTests();
#endif
    std::cerr << "Test [--raster elevation.tif | --scene terrain.osgt elevation.tif]\n";
    return 1;
}
