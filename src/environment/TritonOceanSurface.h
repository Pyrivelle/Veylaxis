#pragma once

#include <osg/Drawable>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Texture2D>
#include <osg/ref_ptr>

#include <QString>
#include <QVector3D>

namespace Triton
{
class Environment;
class Ocean;
class ResourceLoader;
}

struct TritonOceanSurfaceOptions
{
    QString licenseUser;
    QString licenseCode;
    QString resourcePath = QStringLiteral("D:/SDK/Triton/Resources/");
    double windSpeedMetersPerSecond = 15.0;
    double windDirectionRadians = 0.3;
    bool enableBreakingWaves = false;
    bool automaticBreakingWaveDirection = true;
    double breakingWaveWavelengthMeters = 20.0;
    double breakingWaveAmplitudeMeters = 1.0;
    double breakingWaveSurgeDepthMeters = 4.0;
    double breakingWaveDepthFalloff = 2.0;
};

struct TritonOceanSurfaceSettings
{
    double windSpeedMetersPerSecond = 15.0;
    double windDirectionDegrees = 17.0;
    double choppiness = 1.5;
    double seaLevel = 0.0;
    double aboveWaterVisibilityMeters = 30000.0;
    double belowWaterVisibilityMeters = 1000.0;
    double waterTransparency = 0.9;
    QVector3D lightDirection = QVector3D(0.0f, 0.707f, 0.707f);
    QVector3D directLightColor = QVector3D(1.0f, 1.0f, 1.0f);
    QVector3D ambientLightColor = QVector3D(0.25f, 0.32f, 0.35f);
    bool useExternalLighting = false;
};

class TritonOceanSurface : public osg::Geode
{
  public:
    explicit TritonOceanSurface(const TritonOceanSurfaceOptions& options = {});
    ~TritonOceanSurface() override;

    TritonOceanSurfaceSettings settings() const;
    bool isInitialized() const;
    bool initializationFailed() const;
    void applySettings(const TritonOceanSurfaceSettings& settings);
    void setWindSpeedMetersPerSecond(double speed);
    void setWindDirectionDegrees(double degrees);
    void setChoppiness(double choppiness);
    void setSeaLevel(double seaLevel);
    void setAboveWaterVisibilityMeters(double meters);
    void setBelowWaterVisibilityMeters(double meters);
    void setWaterTransparency(double transparency);
    void setHeightMap(osg::Texture2D* texture, osg::Camera* heightMapCamera);
    void setExternalLighting(const QVector3D& direction, const QVector3D& directColor,
                             const QVector3D& ambientColor);

  private:
    class OceanDrawable;

    TritonOceanSurfaceSettings m_settings;
    osg::ref_ptr<OceanDrawable> m_drawable;
};
