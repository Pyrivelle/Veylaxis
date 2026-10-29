#include <osg/observer_ptr>
#include <OpenThreads/ScopedLock>
#include <OpenThreads/Mutex>
#include <algorithm>
#include <set>
#include "TritonOceanSurface.h"

#include "AppLog.h"
#include "RenderBinDefinitions.h"

#include <Triton.h>

#include <osg/BoundingBox>
#include <osg/Camera>
#include <osg/Light>
#include <osg/Notify>
#include <osg/Program>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osg/Vec4>
#include <osgViewer/View>

#include <QDir>

#include <cmath>
#include <cstdint>

namespace
{
QString envText(const char* key)
{
    return QString::fromLocal8Bit(qgetenv(key));
}

std::string normalizedResourcePath(const QString& path)
{
    QString cleaned = QDir::fromNativeSeparators(path);
    if (!cleaned.endsWith('/'))
        cleaned += '/';
    return cleaned.toLocal8Bit().constData();
}

double degreesToRadians(double degrees)
{
    return degrees * 3.14159265358979323846 / 180.0;
}
}

class TritonOceanSurface::OceanDrawable : public osg::Drawable
{
  public:
    explicit OceanDrawable(const TritonOceanSurfaceOptions& options) : m_options(options)
    {
        setDataVariance(osg::Object::DYNAMIC);
        setUseVertexBufferObjects(false);
        setUseDisplayList(false);
        setCullingActive(false);
        m_settings.windSpeedMetersPerSecond = options.windSpeedMetersPerSecond;
        m_settings.windDirectionDegrees = options.windDirectionRadians * 180.0 / 3.14159265358979323846;
    }

    OceanDrawable(const OceanDrawable& other, const osg::CopyOp& copyOp)
        : osg::Drawable(other, copyOp), m_options(other.m_options)
    {
        setDataVariance(osg::Object::DYNAMIC);
        setUseVertexBufferObjects(false);
        setUseDisplayList(false);
        setCullingActive(false);
    }

    bool isSameKindAs(const osg::Object* object) const override
    {
        return dynamic_cast<const OceanDrawable*>(object) != nullptr;
    }

    osg::Object* cloneType() const override
    {
        return new OceanDrawable(TritonOceanSurfaceOptions());
    }

    osg::Object* clone(const osg::CopyOp& copyOp) const override
    {
        return new OceanDrawable(*this, copyOp);
    }

    ~OceanDrawable() override
    {
        cleanup();
    }

    osg::BoundingBox computeBoundingBox() const override
    {
        const float extent = 100000.0f;
        return osg::BoundingBox(-extent, -extent, -1000.0f, extent, extent, 1000.0f);
    }

    void drawImplementation(osg::RenderInfo& renderInfo) const override
    {
        osg::State& state = *renderInfo.getState();
        state.disableAllVertexArrays();

        if (!m_ocean)
            const_cast<OceanDrawable*>(this)->setup();

        if (m_environment)
            const_cast<OceanDrawable*>(this)->installHeightMap(renderInfo);

        if (m_environment)
        {
            const_cast<OceanDrawable*>(this)->applySettingsToTriton();

            if (osg::Camera* camera = renderInfo.getCurrentCamera())
                m_environment->SetCameraMatrix(camera->getViewMatrix().ptr());
            m_environment->SetProjectionMatrix(state.getProjectionMatrix().ptr());

            TritonOceanSurfaceSettings settings;
            {
                OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
                settings = m_settings;
            }
            if (settings.useExternalLighting)
            {
                m_environment->SetDirectionalLight(
                    Triton::Vector3(settings.lightDirection.x(), settings.lightDirection.y(),
                                    settings.lightDirection.z()),
                    Triton::Vector3(settings.directLightColor.x(), settings.directLightColor.y(),
                                    settings.directLightColor.z()));
                m_environment->SetAmbientLight(Triton::Vector3(settings.ambientLightColor.x(),
                                                               settings.ambientLightColor.y(),
                                                               settings.ambientLightColor.z()));
            }
            else
            {
                const Triton::Vector3 lightPosition(0.0, 1.0 / std::sqrt(2.0), 1.0 / std::sqrt(2.0));
                m_environment->SetDirectionalLight(lightPosition, Triton::Vector3(1.0, 1.0, 1.0));

                Triton::Vector3 ambientColor(67.0 / 255.0, 102.0 / 255.0, 104.0 / 255.0);
                ambientColor.Normalize();
                m_environment->SetAmbientLight(ambientColor);
            }
            m_environment->SetBelowWaterVisibility(settings.belowWaterVisibilityMeters,
                                                   Triton::Vector3(0.0, 0.1, 0.3));
        }

        if (m_ocean)
        {
            double timeSeconds = 0.0;
            if (renderInfo.getView() && renderInfo.getView()->getFrameStamp())
                timeSeconds = renderInfo.getView()->getFrameStamp()->getSimulationTime();

            glDepthFunc(GL_LEQUAL);
            m_ocean->Draw(timeSeconds);
        }

        // Triton changes the active GLSL program and other raw OpenGL state
        // outside OSG's state cache. Invalidate the cache before subsequent
        // model drawables render, otherwise terrain can inherit Triton's ocean
        // shader and produce black bands or other screen-space artifacts.
        state.dirtyAllVertexArrays();
        state.dirtyAllModes();
        state.dirtyAllAttributes();
        state.setLastAppliedProgramObject(nullptr);
        state.applyAttribute(new osg::Program());
    }

    bool isInitialized() const
    {
        return m_ocean != nullptr;
    }

    bool initializationFailed() const
    {
        return m_setupAttempted && !m_ocean;
    }

    TritonOceanSurfaceSettings settings() const
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
        return m_settings;
    }

    void applySettings(const TritonOceanSurfaceSettings& settings)
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
        m_settings = settings;
        m_settingsDirty = true;
    }

    void setHeightMap(osg::Texture2D* texture, osg::Camera* camera)
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
        m_heightMapTexture = texture;
        m_heightMapCamera = camera;
        m_heightMapContextIds.clear();
    }

  private:
    void setup()
    {
        if (m_setupAttempted)
            return;
        m_setupAttempted = true;

        const std::string resourcePath = normalizedResourcePath(m_options.resourcePath);
        m_resourceLoader = new Triton::ResourceLoader(resourcePath.c_str());

        m_environment = new Triton::Environment();
        const Triton::EnvironmentError error =
            m_environment->Initialize(Triton::FLAT_ZUP, Triton::OPENGL_2_0, m_resourceLoader);
        if (error != Triton::SUCCEEDED)
        {
            Msg() << QStringLiteral("[TritonOcean] Initialize failed. resourcePath=%1 error=%2")
                         .arg(m_options.resourcePath)
                         .arg(static_cast<int>(error))
                  << std::endl;
            cleanup();
            return;
        }

        QString licenseUser = m_options.licenseUser.trimmed();
        QString licenseCode = m_options.licenseCode.trimmed();
        if (licenseUser.isEmpty())
            licenseUser = envText("TRITON_LICENSE_USER");
        if (licenseCode.isEmpty())
            licenseCode = envText("TRITON_LICENSE_CODE");

        m_environment->SetLicenseCode(licenseUser.toLocal8Bit().constData(),
                                      licenseCode.toLocal8Bit().constData());

        Triton::WindFetch windFetch;
        windFetch.SetWind(m_settings.windSpeedMetersPerSecond,
                          degreesToRadians(m_settings.windDirectionDegrees));
        m_environment->AddWindFetch(windFetch);

        if (m_options.enableBreakingWaves)
        {
            Triton::BreakingWavesParameters breakingWaves;
            breakingWaves.SetAutoWaveDirection(m_options.automaticBreakingWaveDirection);
            breakingWaves.SetWavelength(static_cast<float>(m_options.breakingWaveWavelengthMeters));
            breakingWaves.SetAmplitude(static_cast<float>(m_options.breakingWaveAmplitudeMeters));
            breakingWaves.SetSurgeDepth(static_cast<float>(m_options.breakingWaveSurgeDepthMeters));
            breakingWaves.SetDepthFalloff(static_cast<float>(m_options.breakingWaveDepthFalloff));
            m_environment->SetBreakingWavesParameters(breakingWaves);
        }

        m_ocean = Triton::Ocean::Create(m_environment, Triton::JONSWAP, false, m_options.enableBreakingWaves);
        if (!m_ocean)
        {
            Msg()
                << QStringLiteral(
                       "[TritonOcean] Ocean creation failed. Enable Triton.config debug messages for details.")
                << std::endl;
            cleanup();
            return;
        }

        Msg()
            << QStringLiteral("[TritonOcean] Ocean initialized. resourcePath=%1").arg(m_options.resourcePath)
            << std::endl;
        m_settingsDirty = true;
        applySettingsToTriton();
    }

    void installHeightMap(osg::RenderInfo& renderInfo)
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
        if (!m_environment || !m_heightMapTexture || !m_heightMapCamera)
            return;

        const unsigned int contextId = renderInfo.getContextID();
        if (m_heightMapContextIds.count(contextId) != 0)
            return;

        osg::Texture::TextureObject* textureObject = m_heightMapTexture->getTextureObject(contextId);
        if (!textureObject)
            return;

        const osg::Matrixd bias(0.5, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.5, 0.5, 0.5,
                                1.0);
        const osg::Matrixd worldToTexture =
            m_heightMapCamera->getViewMatrix() * m_heightMapCamera->getProjectionMatrix() * bias;
        m_environment->SetHeightMap(
            reinterpret_cast<Triton::TextureHandle>(static_cast<std::uintptr_t>(textureObject->id())),
            Triton::Matrix4(worldToTexture.ptr()));
        m_heightMapContextIds.insert(contextId);
        Msg() << QStringLiteral("[TritonOcean] Height map installed. context=%1 texture=%2")
                     .arg(contextId)
                     .arg(textureObject->id())
              << std::endl;
    }

    void applySettingsToTriton()
    {
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
        if (!m_settingsDirty || !m_environment)
            return;

        m_environment->ClearWindFetches();
        Triton::WindFetch windFetch;
        windFetch.SetWind(m_settings.windSpeedMetersPerSecond,
                          degreesToRadians(m_settings.windDirectionDegrees));
        m_environment->AddWindFetch(windFetch);
        m_environment->SetSeaLevel(m_settings.seaLevel);
        m_environment->SetAboveWaterVisibility(m_settings.aboveWaterVisibilityMeters,
                                               Triton::Vector3(0.72, 0.82, 0.92));
        m_environment->SetWaterTransparency(static_cast<float>(m_settings.waterTransparency));

        if (m_ocean)
            m_ocean->SetChoppiness(static_cast<float>(m_settings.choppiness));

        m_settingsDirty = false;
    }

    void cleanup()
    {
        if (m_ocean)
        {
            delete m_ocean;
            m_ocean = nullptr;
        }
        if (m_environment)
        {
            delete m_environment;
            m_environment = nullptr;
        }
        if (m_resourceLoader)
        {
            delete m_resourceLoader;
            m_resourceLoader = nullptr;
        }
    }

    TritonOceanSurfaceOptions m_options;
    TritonOceanSurfaceSettings m_settings;
    mutable OpenThreads::Mutex m_mutex;
    bool m_setupAttempted = false;
    bool m_settingsDirty = true;
    osg::ref_ptr<osg::Texture2D> m_heightMapTexture;
    osg::observer_ptr<osg::Camera> m_heightMapCamera;
    std::set<unsigned int> m_heightMapContextIds;
    Triton::ResourceLoader* m_resourceLoader = nullptr;
    Triton::Environment* m_environment = nullptr;
    Triton::Ocean* m_ocean = nullptr;
};

TritonOceanSurface::TritonOceanSurface(const TritonOceanSurfaceOptions& options)
{
    setName("Triton Ocean");
    m_settings.windSpeedMetersPerSecond = options.windSpeedMetersPerSecond;
    m_settings.windDirectionDegrees = options.windDirectionRadians * 180.0 / 3.14159265358979323846;
    m_drawable = new OceanDrawable(options);
    addDrawable(m_drawable.get());
    setCullingActive(false);
    getOrCreateStateSet()->setRenderBinDetails(FARSEA_RENDER_BIN_OCEAN, FARSEA_RENDER_BIN_NAME);
}

TritonOceanSurface::~TritonOceanSurface() = default;

bool TritonOceanSurface::isInitialized() const
{
    return m_drawable->isInitialized();
}

bool TritonOceanSurface::initializationFailed() const
{
    return m_drawable->initializationFailed();
}

TritonOceanSurfaceSettings TritonOceanSurface::settings() const
{
    return m_drawable ? m_drawable->settings() : m_settings;
}

void TritonOceanSurface::applySettings(const TritonOceanSurfaceSettings& settings)
{
    m_settings = settings;
    if (m_drawable)
        m_drawable->applySettings(settings);
}

void TritonOceanSurface::setWindSpeedMetersPerSecond(double speed)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.windSpeedMetersPerSecond = speed;
    applySettings(updated);
}

void TritonOceanSurface::setWindDirectionDegrees(double degrees)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.windDirectionDegrees = degrees;
    applySettings(updated);
}

void TritonOceanSurface::setChoppiness(double choppiness)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.choppiness = choppiness;
    applySettings(updated);
}

void TritonOceanSurface::setSeaLevel(double seaLevel)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.seaLevel = seaLevel;
    applySettings(updated);
}

void TritonOceanSurface::setAboveWaterVisibilityMeters(double meters)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.aboveWaterVisibilityMeters = meters;
    applySettings(updated);
}

void TritonOceanSurface::setBelowWaterVisibilityMeters(double meters)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.belowWaterVisibilityMeters = meters;
    applySettings(updated);
}

void TritonOceanSurface::setWaterTransparency(double transparency)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.waterTransparency = transparency;
    applySettings(updated);
}

void TritonOceanSurface::setHeightMap(osg::Texture2D* texture, osg::Camera* heightMapCamera)
{
    if (m_drawable)
        m_drawable->setHeightMap(texture, heightMapCamera);
}

void TritonOceanSurface::setExternalLighting(const QVector3D& direction, const QVector3D& directColor,
                                             const QVector3D& ambientColor)
{
    TritonOceanSurfaceSettings updated = settings();
    updated.lightDirection = direction;
    if (!updated.lightDirection.isNull())
        updated.lightDirection.normalize();
    updated.directLightColor = directColor;
    updated.ambientLightColor = ambientColor;
    updated.useExternalLighting = true;
    applySettings(updated);
}
