#pragma once

#include <osg/Geode>
#include <osg/ref_ptr>
#include <osgViewer/Viewer>

#include <QString>
#include <QVector3D>

namespace SilverLining
{
class Atmosphere;
}

struct SilverLiningSkyBoxOptions
{
    QString licenseUser;
    QString licenseCode;
    QString resourcePath = QStringLiteral("D:/SDK/SilverLining/resources/");
    double latitude = 38.9167;
    double longitude = 121.6;
    double altitudeMeters = 0.0;
    double skyboxSize = 100000.0;
    bool drawSunMoon = true;
};

struct SilverLiningSkyBoxSettings
{
    double hour = 12.0;
    double rainRate = 0.0;
    double snowRate = 0.0;
    double fogDensity = 0.0;
    double visibilityMeters = 30000.0;
    double cloudBaseAltitudeMeters = 4000.0;
    double cloudThicknessMeters = 500.0;
    double cloudDensity = 0.8;
};

struct SilverLiningLightingState
{
    QVector3D direction = QVector3D(0.0f, 0.707f, 0.707f);
    QVector3D directColor = QVector3D(1.0f, 1.0f, 1.0f);
    QVector3D ambientColor = QVector3D(0.25f, 0.32f, 0.35f);
    bool valid = false;
};

class SilverLiningSkyBox : public osg::Geode
{
  public:
    class AtmosphereReference;

    SilverLiningSkyBox(osgViewer::Viewer* viewer, osg::Group* sceneRoot,
                       const SilverLiningSkyBoxOptions& options = {});
    ~SilverLiningSkyBox() override;

    SilverLiningSkyBoxSettings settings() const;
    SilverLiningLightingState lightingState() const;
    bool initializationFailed() const;
    void applySettings(const SilverLiningSkyBoxSettings& settings);
    void setRainRate(double rate);
    void setHour(double hour);
    void setSnowRate(double rate);
    void setFogDensity(double density);
    void setVisibilityMeters(double meters);
    void setCloudBaseAltitudeMeters(double meters);
    void setCloudThicknessMeters(double meters);
    void setCloudDensity(double density);

  private:
    class SkyDrawable;
    class CloudDrawable;

    void applySettingsToAtmosphere();

    SilverLiningSkyBoxSettings m_settings;
    osg::ref_ptr<AtmosphereReference> m_atmosphereReference;
    osg::ref_ptr<SkyDrawable> m_skyDrawable;
    osg::ref_ptr<CloudDrawable> m_cloudDrawable;
};
