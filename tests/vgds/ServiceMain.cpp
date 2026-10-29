#include <string>
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
    if (argc > 1)
    {
        std::string mode = argv[1];
        if (mode == "--acceptance")
            return runAcceptanceTests();
        if (mode == "--seabed-maritime")
            return runSeabedMaritimeTests();
        if (mode == "--island")
            return runIslandTerrainTests();
        if (mode == "--catalog")
            return runCatalogTests();
        if (mode == "--vgds")
            return runVgdsTests();
        if (mode == "--s57")
            return runVgdsS57Tests();
#ifdef VEYLAXIS_HAS_OSGB
        if (mode == "--streaming")
            return runVgdsStreamingTests();
        if (std::string(argv[1]) == "--navigation-models")
            return runNavigationModelTests();
        if (std::string(argv[1]) == "--dmu-models")
            return runDmuModelTests();
#endif
    }
    return runVgdsServiceTests();
}
