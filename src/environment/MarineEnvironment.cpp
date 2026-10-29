// 【阅读导航】环境装配与资源入口：组织天空和海洋、传递光照，读取运行目录中的 SDK 资源和本机配置。
#include "MarineEnvironment.h"
#include "veylaxis/RuntimeEnvironment.hpp"
#include <Windows.h>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <filesystem>
#include <stdexcept>
#include <iostream>

MarineEnvironment::MarineEnvironment(osgViewer::Viewer* viewer, osg::Group* root, double longitude,
                                     double latitude)
    : viewer_(viewer), root_(root)
{
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    if (!count || count >= 32768)
        throw std::runtime_error("Cannot locate marine runtime directory");
    const QString appDir = QString::fromStdWString(std::filesystem::path(executable).parent_path().wstring());
    veylaxis::loadRuntimeEnvironment();
    for (const char* key :
         {"SILVERLINING_USER", "SILVERLINING_LICENSE_CODE", "TRITON_LICENSE_USER", "TRITON_LICENSE_CODE"})
    {
        if (qgetenv(key).isEmpty())
            throw std::runtime_error(std::string("Missing marine SDK configuration: ") + key);
    }
    const QString resource = appDir + "/resource/";
    for (const char* file :
         {"SilverLining/resources/SilverLining.config",
          "SilverLining/resources/VC14/x64/SilverLiningOpenGL-MT-DLL.dll", "Triton/Resources/Triton.config"})
        if (!QFileInfo::exists(resource + file))
            throw std::runtime_error(std::string("Missing marine resource: ") + file);
    SilverLiningSkyBoxOptions skyOptions;
    skyOptions.resourcePath = resource + "SilverLining/resources/";
    skyOptions.longitude = longitude;
    skyOptions.latitude = latitude;
    sky_ = new SilverLiningSkyBox(viewer, root, skyOptions);
    sky_->setHour(16.0); // Demonstration daylight at this US east coast location (UTC).
    root->addChild(sky_);
    TritonOceanSurfaceOptions oceanOptions;
    oceanOptions.resourcePath = resource + "Triton/Resources/";
    oceanOptions.windSpeedMetersPerSecond = 8.0;
    ocean_ = new TritonOceanSurface(oceanOptions);
    ocean_->setSeaLevel(0.0);
    root->addChild(ocean_);
    std::cout << "Marine scene: SilverLining + Triton; sea level Z=0 m; terrain XYZ scale 1:1; location "
              << longitude << ", " << latitude << "\n"
              << std::flush;
}

// 天空提供光照方向与颜色，海洋消费同一光照；无需让数据服务感知环境渲染。
void MarineEnvironment::updateLighting()
{
    const auto light = sky_->lightingState();
    if (light.valid)
        ocean_->setExternalLighting(light.direction, light.directColor, light.ambientColor);
}

bool MarineEnvironment::ready() const
{
    return sky_ && ocean_ && sky_->lightingState().valid && ocean_->isInitialized();
}

bool MarineEnvironment::failed() const
{
    return sky_->initializationFailed() || ocean_->initializationFailed();
}

void MarineEnvironment::toggleOcean()
{
    ocean_->setNodeMask(ocean_->getNodeMask() ? 0 : ~0u);
}

void MarineEnvironment::shutdown()
{
    if (viewer_.valid())
        viewer_->getCamera()->setUserData(nullptr);
    if (root_.valid())
    {
        root_->removeChild(ocean_);
        root_->removeChild(sky_);
    }
    ocean_ = nullptr;
    sky_ = nullptr;
}
