#include <osg/observer_ptr>
#include <OpenThreads/ScopedLock>
#include <OpenThreads/Mutex>
#include <algorithm>
#include <set>
#include "SilverLiningSkyBox.h"

#include "AppLog.h"
#include "RenderBinDefinitions.h"

#include <QDir>

#include <OpenThreads/Mutex>
#include <osg/Depth>
#include <osg/Light>
#include <osg/State>
#include <osg/StateSet>
#include <osgViewer/Viewer>

#include <SilverLining.h>

#include <cmath>

class SilverLiningSkyBox::AtmosphereReference : public osg::Referenced
{
  public:
    AtmosphereReference(const QString& user, const QString& code)
    {
        atmosphere =
            new SilverLining::Atmosphere(user.toLocal8Bit().constData(), code.toLocal8Bit().constData());
    }

    ~AtmosphereReference() override
    {
        delete atmosphere;
        atmosphere = nullptr;
        cloudLayer = nullptr;
    }

    SilverLining::Atmosphere* atmosphere = nullptr;
    SilverLining::CloudLayer* cloudLayer = nullptr;
    bool atmosphereInitialized = false;
    bool initializationAttempted = false;
};

namespace
{
QString envText(const char* key)
{
    return QString::fromLocal8Bit(qgetenv(key));
}

std::string normalizedResourcePath(const QString& path)
{
    QString normalized = QDir::fromNativeSeparators(path.trimmed());
    if (!normalized.endsWith(QLatin1Char('/')))
        normalized += QLatin1Char('/');
    return normalized.toStdString();
}

double normalizedHour(double hour)
{
    while (hour < 0.0)
        hour += 24.0;
    while (hour >= 24.0)
        hour -= 24.0;
    return hour;
}

class SilverLiningCullCallback : public osg::Drawable::CullCallback
{
  public:
    bool cull(osg::NodeVisitor*, osg::Drawable*, osg::RenderInfo*) const override
    {
        if (atmosphere)
            atmosphere->CullObjects();
        return false;
    }

    SilverLining::Atmosphere* atmosphere = nullptr;
};

class SilverLiningSkyUpdateCallback : public osg::Drawable::UpdateCallback
{
  public:
    explicit SilverLiningSkyUpdateCallback(osg::Camera* camera) : m_camera(camera)
    {
    }

    void update(osg::NodeVisitor*, osg::Drawable* drawable) override
    {
        if (!m_camera.valid() || !drawable)
            return;

        auto* reference = dynamic_cast<SilverLiningSkyBox::AtmosphereReference*>(m_camera->getUserData());
        if (!reference || !reference->atmosphereInitialized)
            return;

        drawable->dirtyBound();
    }

  private:
    osg::observer_ptr<osg::Camera> m_camera;
};

class SilverLiningSkyBoundsCallback : public osg::Drawable::ComputeBoundingBoxCallback
{
  public:
    SilverLiningSkyBoundsCallback(osg::Camera* camera, double skyboxSize)
        : m_camera(camera), m_skyboxSize(skyboxSize)
    {
    }

    osg::BoundingBox computeBound(const osg::Drawable&) const override
    {
        osg::BoundingBox box;
        if (!m_camera.valid())
            return box;

        auto* reference = dynamic_cast<SilverLiningSkyBox::AtmosphereReference*>(m_camera->getUserData());
        SilverLining::Atmosphere* atmosphere = reference ? reference->atmosphere : nullptr;
        if (!atmosphere)
            return box;

        double skyboxSize = m_skyboxSize;
        if (skyboxSize <= 0.0)
            skyboxSize = atmosphere->GetConfigOptionDouble("sky-box-size");
        if (skyboxSize <= 0.0)
            skyboxSize = 1000.0;

        osg::Vec3d eye;
        osg::Vec3d center;
        osg::Vec3d up;
        m_camera->getViewMatrixAsLookAt(eye, center, up);

        const osg::Vec3d radius(skyboxSize * 0.5, skyboxSize * 0.5, skyboxSize * 0.5);
        box.set(eye - radius, eye + radius);
        return box;
    }

  private:
    osg::observer_ptr<osg::Camera> m_camera;
    double m_skyboxSize = 100000.0;
};

class SilverLiningCloudsUpdateCallback : public osg::Drawable::UpdateCallback
{
  public:
    void update(osg::NodeVisitor*, osg::Drawable* drawable) override
    {
        if (drawable)
            drawable->dirtyBound();
    }
};

class SilverLiningCloudsBoundsCallback : public osg::Drawable::ComputeBoundingBoxCallback
{
  public:
    explicit SilverLiningCloudsBoundsCallback(osg::Camera* camera) : m_camera(camera)
    {
    }

    osg::BoundingBox computeBound(const osg::Drawable&) const override
    {
        osg::BoundingBox box;
        if (!m_camera.valid())
            return box;

        auto* reference = dynamic_cast<SilverLiningSkyBox::AtmosphereReference*>(m_camera->getUserData());
        SilverLining::Atmosphere* atmosphere = reference ? reference->atmosphere : nullptr;
        if (!atmosphere)
            return box;

        double minX = 0.0;
        double minY = 0.0;
        double minZ = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;
        double maxZ = 0.0;
        atmosphere->GetCloudBounds(minX, minY, minZ, maxX, maxY, maxZ);
        box.set(osg::Vec3d(minX, minY, minZ), osg::Vec3d(maxX, maxY, maxZ));
        return box;
    }

  private:
    osg::observer_ptr<osg::Camera> m_camera;
};
}

class SilverLiningSkyBox::SkyDrawable : public osg::Drawable
{
  public:
    SkyDrawable() = default;

    SkyDrawable(SilverLiningSkyBox* owner, osgViewer::Viewer* viewer,
                const SilverLiningSkyBoxOptions& options)
        : m_viewer(viewer), m_owner(owner), m_options(options)
    {
        setDataVariance(osg::Object::DYNAMIC);
        setUseVertexBufferObjects(false);
        setUseDisplayList(false);

        m_cullCallback = new SilverLiningCullCallback();
        setCullCallback(m_cullCallback.get());
        if (viewer && viewer->getCamera())
        {
            setUpdateCallback(new SilverLiningSkyUpdateCallback(viewer->getCamera()));
            setComputeBoundingBoxCallback(
                new SilverLiningSkyBoundsCallback(viewer->getCamera(), options.skyboxSize));
        }
    }

    bool isSameKindAs(const osg::Object* object) const override
    {
        return dynamic_cast<const SkyDrawable*>(object) != nullptr;
    }

    osg::Object* cloneType() const override
    {
        return new SkyDrawable();
    }

    osg::Object* clone(const osg::CopyOp&) const override
    {
        return new SkyDrawable();
    }

    void drawImplementation(osg::RenderInfo& renderInfo) const override
    {
        auto* reference = dynamic_cast<AtmosphereReference*>(renderInfo.getCurrentCamera()->getUserData());
        SilverLining::Atmosphere* atmosphere = reference ? reference->atmosphere : nullptr;
        if (!atmosphere)
            return;

        renderInfo.getState()->disableAllVertexArrays();

        initializeSilverLining(reference);
        if (reference->atmosphereInitialized)
        {
            const osg::Matrix projectionMatrix = renderInfo.getState()->getProjectionMatrix();
            const osg::Matrix viewMatrix = renderInfo.getCurrentCamera()->getViewMatrix();
            atmosphere->SetProjectionMatrix(projectionMatrix.ptr());
            atmosphere->SetCameraMatrix(viewMatrix.ptr());

            atmosphere->EnableLensFlare(m_options.drawSunMoon);
            atmosphere->DrawSky(true, false, m_options.skyboxSize, true, false, m_options.drawSunMoon);
            setLighting(atmosphere);
        }

        renderInfo.getState()->dirtyAllVertexArrays();
    }

  private:
    void initializeSilverLining(AtmosphereReference* reference) const
    {
        if (!reference || reference->initializationAttempted || !reference->atmosphere)
            return;

        reference->initializationAttempted = true;
        SilverLining::Atmosphere* atmosphere = reference->atmosphere;
        srand(1234);

        const std::string resourcePath = normalizedResourcePath(m_options.resourcePath);
        const int result =
            atmosphere->Initialize(SilverLining::Atmosphere::OPENGL, resourcePath.c_str(), true, nullptr);
        if (result != SilverLining::Atmosphere::E_NOERROR)
        {
            Msg() << QStringLiteral("[SilverLiningSkyBox] Initialize failed, code=%1 resourcePath=%2")
                         .arg(result)
                         .arg(QString::fromStdString(resourcePath))
                  << std::endl;
            return;
        }

        reference->atmosphereInitialized = true;
        atmosphere->SetUpVector(0.0, 0.0, 1.0);
        atmosphere->SetRightVector(1.0, 0.0, 0.0);

        SilverLining::Location location;
        location.SetAltitude(m_options.altitudeMeters);
        location.SetLatitude(m_options.latitude);
        location.SetLongitude(m_options.longitude);
        atmosphere->GetConditions()->SetLocation(location);

        SilverLining::LocalTime time;
        time.SetFromSystemTime();
        time.SetHour(m_owner ? m_owner->m_settings.hour : 12.0);
        time.SetTimeZone(0.0);
        atmosphere->GetConditions()->SetTime(time);

        installDefaultCloudLayer(atmosphere);

        if (m_cullCallback.valid())
            m_cullCallback->atmosphere = atmosphere;

        Msg() << QStringLiteral("[SilverLiningSkyBox] Initialized with official OSG sample path.")
              << std::endl;
    }

    void installDefaultCloudLayer(SilverLining::Atmosphere* atmosphere) const
    {
        if (!atmosphere)
            return;

        osg::Vec3d position;
        if (m_viewer.valid() && m_viewer->getCameraManipulator())
            position = m_viewer->getCameraManipulator()->getMatrix().getTrans();

        SilverLining::CloudLayer* cumulusLayer =
            SilverLining::CloudLayerFactory::Create(CUMULUS_CONGESTUS, *atmosphere);
        if (!cumulusLayer)
            return;

        cumulusLayer->SetIsInfinite(true);
        cumulusLayer->SetBaseAltitude(4000.0);
        cumulusLayer->SetThickness(500.0);
        cumulusLayer->SetBaseLength(40000.0);
        cumulusLayer->SetBaseWidth(40000.0);
        cumulusLayer->SetDensity(0.8);
        cumulusLayer->SetAlpha(0.8);
        cumulusLayer->SetFadeTowardEdges(true);
        cumulusLayer->SetLayerPosition(position.x(), -position.y());
        cumulusLayer->SeedClouds(*atmosphere);
        atmosphere->GetConditions()->AddCloudLayer(cumulusLayer);
        if (m_owner)
            m_owner->m_atmosphereReference->cloudLayer = cumulusLayer;
        if (m_owner)
            m_owner->applySettingsToAtmosphere();
    }

    void setLighting(SilverLining::Atmosphere* atmosphere) const
    {
        if (!m_viewer.valid() || !atmosphere)
            return;

        osg::Light* light = m_viewer->getLight();
        if (!light)
            return;

        float ambientR = 0.0f;
        float ambientG = 0.0f;
        float ambientB = 0.0f;
        float diffuseR = 0.0f;
        float diffuseG = 0.0f;
        float diffuseB = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        atmosphere->GetAmbientColor(&ambientR, &ambientG, &ambientB);
        atmosphere->GetSunOrMoonColor(&diffuseR, &diffuseG, &diffuseB);
        atmosphere->GetSunOrMoonPosition(&x, &y, &z);

        osg::Vec3 direction(x, y, z);
        direction.normalize();

        light->setAmbient(osg::Vec4(ambientR, ambientG, ambientB, 1.0f));
        light->setDiffuse(osg::Vec4(diffuseR, diffuseG, diffuseB, 1.0f));
        light->setSpecular(osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        light->setPosition(osg::Vec4(direction.x(), direction.y(), direction.z(), 0.0f));
    }

    osg::observer_ptr<osgViewer::Viewer> m_viewer;
    SilverLiningSkyBox* m_owner = nullptr;
    SilverLiningSkyBoxOptions m_options;
    osg::ref_ptr<SilverLiningCullCallback> m_cullCallback;
};

class SilverLiningSkyBox::CloudDrawable : public osg::Drawable
{
  public:
    CloudDrawable() = default;

    explicit CloudDrawable(osgViewer::Viewer* viewer) : m_viewer(viewer)
    {
        setDataVariance(osg::Object::DYNAMIC);
        setUseVertexBufferObjects(false);
        setUseDisplayList(false);
        if (viewer && viewer->getCamera())
        {
            setUpdateCallback(new SilverLiningCloudsUpdateCallback());
            setComputeBoundingBoxCallback(new SilverLiningCloudsBoundsCallback(viewer->getCamera()));
        }
    }

    bool isSameKindAs(const osg::Object* object) const override
    {
        return dynamic_cast<const CloudDrawable*>(object) != nullptr;
    }

    osg::Object* cloneType() const override
    {
        return new CloudDrawable();
    }

    osg::Object* clone(const osg::CopyOp&) const override
    {
        return new CloudDrawable();
    }

    void drawImplementation(osg::RenderInfo& renderInfo) const override
    {
        auto* reference = dynamic_cast<AtmosphereReference*>(renderInfo.getCurrentCamera()->getUserData());
        SilverLining::Atmosphere* atmosphere = reference ? reference->atmosphere : nullptr;
        if (!atmosphere || !reference->atmosphereInitialized)
            return;

        renderInfo.getState()->disableAllVertexArrays();
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(m_mutex);
        atmosphere->DrawObjects(true);
        renderInfo.getState()->dirtyAllVertexArrays();
    }

  private:
    osg::observer_ptr<osgViewer::Viewer> m_viewer;
    mutable OpenThreads::Mutex m_mutex;
};

SilverLiningSkyBox::SilverLiningSkyBox(osgViewer::Viewer* viewer, osg::Group* sceneRoot,
                                       const SilverLiningSkyBoxOptions& options)
{
    setName("SilverLining SkyBox");
    setCullingActive(false);

    QString licenseUser = options.licenseUser.trimmed();
    QString licenseCode = options.licenseCode.trimmed();
    if (licenseUser.isEmpty())
        licenseUser = envText("SILVERLINING_USER");
    if (licenseCode.isEmpty())
        licenseCode = envText("SILVERLINING_LICENSE_CODE");

    osg::ref_ptr<AtmosphereReference> reference = new AtmosphereReference(licenseUser, licenseCode);
    m_atmosphereReference = reference;
    if (viewer && viewer->getCamera())
    {
        viewer->getCamera()->setUserData(reference.get());
        viewer->getCamera()->setClearMask(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        // The owning scene runtime controls the main camera clipping policy.
        // Overriding it here made fixed near/far planes change while navigating.
        viewer->setLightingMode(osgViewer::View::SKY_LIGHT);
    }

    if (sceneRoot)
        sceneRoot->getOrCreateStateSet()->setRenderBinDetails(FARSEA_RENDER_BIN_MODELS,
                                                              FARSEA_RENDER_BIN_NAME);

    osg::StateSet* stateSet = getOrCreateStateSet();
    stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
    stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);

    m_skyDrawable = new SkyDrawable(this, viewer, options);
    m_skyDrawable->getOrCreateStateSet()->setRenderBinDetails(FARSEA_RENDER_BIN_SKY, FARSEA_RENDER_BIN_NAME);
    m_skyDrawable->getOrCreateStateSet()->setAttributeAndModes(
        new osg::Depth(osg::Depth::LEQUAL, 0.0, 1.0, false));
    addDrawable(m_skyDrawable.get());

    m_cloudDrawable = new CloudDrawable(viewer);
    m_cloudDrawable->getOrCreateStateSet()->setRenderBinDetails(FARSEA_RENDER_BIN_CLOUDS,
                                                                FARSEA_RENDER_BIN_NAME);
    addDrawable(m_cloudDrawable.get());
}

SilverLiningSkyBoxSettings SilverLiningSkyBox::settings() const
{
    return m_settings;
}

SilverLiningLightingState SilverLiningSkyBox::lightingState() const
{
    SilverLiningLightingState state;
    if (!m_atmosphereReference.valid() || !m_atmosphereReference->atmosphere ||
        !m_atmosphereReference->atmosphereInitialized)
        return state;

    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float ar = 0.25f;
    float ag = 0.32f;
    float ab = 0.35f;

    SilverLining::Atmosphere* atmosphere = m_atmosphereReference->atmosphere;
    atmosphere->GetSunOrMoonPosition(&x, &y, &z);
    atmosphere->GetSunOrMoonColor(&r, &g, &b);
    atmosphere->GetAmbientColor(&ar, &ag, &ab);

    QVector3D direction(x, y, z);
    if (!direction.isNull())
        direction.normalize();

    state.direction = direction;
    state.directColor = QVector3D(r, g, b);
    state.ambientColor = QVector3D(ar, ag, ab);
    state.valid = true;
    return state;
}

bool SilverLiningSkyBox::initializationFailed() const
{
    return m_atmosphereReference.valid() && m_atmosphereReference->initializationAttempted &&
           !m_atmosphereReference->atmosphereInitialized;
}

void SilverLiningSkyBox::applySettings(const SilverLiningSkyBoxSettings& settings)
{
    m_settings = settings;
    m_settings.hour = normalizedHour(m_settings.hour);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setRainRate(double rate)
{
    m_settings.rainRate = qBound(0.0, rate, 20.0);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setHour(double hour)
{
    m_settings.hour = normalizedHour(qBound(0.0, hour, 24.0));
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setSnowRate(double rate)
{
    m_settings.snowRate = qBound(0.0, rate, 20.0);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setFogDensity(double density)
{
    m_settings.fogDensity = qBound(0.0, density, 0.2);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setVisibilityMeters(double meters)
{
    m_settings.visibilityMeters = qBound(500.0, meters, 100000.0);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setCloudBaseAltitudeMeters(double meters)
{
    m_settings.cloudBaseAltitudeMeters = qBound(100.0, meters, 12000.0);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setCloudThicknessMeters(double meters)
{
    m_settings.cloudThicknessMeters = qBound(0.0, meters, 5000.0);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::setCloudDensity(double density)
{
    m_settings.cloudDensity = qBound(0.0, density, 1.0);
    applySettingsToAtmosphere();
}

void SilverLiningSkyBox::applySettingsToAtmosphere()
{
    if (!m_atmosphereReference.valid() || !m_atmosphereReference->atmosphere ||
        !m_atmosphereReference->atmosphereInitialized)
        return;

    SilverLining::Atmosphere* atmosphere = m_atmosphereReference->atmosphere;
    SilverLining::LocalTime time;
    time.SetFromSystemTime();
    m_settings.hour = normalizedHour(m_settings.hour);
    time.SetHour(static_cast<int>(std::floor(m_settings.hour)));
    time.SetMinutes(static_cast<int>((m_settings.hour - std::floor(m_settings.hour)) * 60.0));
    time.SetTimeZone(0.0);
    atmosphere->GetConditions()->SetTime(time);
    atmosphere->ForceLightingRecompute();

    atmosphere->GetConditions()->SetVisibility(m_settings.visibilityMeters);

    if (m_settings.fogDensity > 0.0)
        atmosphere->GetConditions()->SetFog(m_settings.fogDensity, 0.72, 0.76, 0.80);
    else
        atmosphere->GetConditions()->ClearFog();

    if (m_settings.rainRate <= 0.0 && m_settings.snowRate <= 0.0)
    {
        atmosphere->GetConditions()->SetPrecipitation(SilverLining::CloudLayer::NONE, 0.0);
    }
    else
    {
        if (m_settings.rainRate > 0.0)
            atmosphere->GetConditions()->SetPrecipitation(SilverLining::CloudLayer::RAIN,
                                                          m_settings.rainRate);
        if (m_settings.snowRate > 0.0)
            atmosphere->GetConditions()->SetPrecipitation(SilverLining::CloudLayer::DRY_SNOW,
                                                          m_settings.snowRate);
    }

    if (m_atmosphereReference->cloudLayer)
    {
        m_atmosphereReference->cloudLayer->SetBaseAltitude(m_settings.cloudBaseAltitudeMeters);
        m_atmosphereReference->cloudLayer->SetThickness(m_settings.cloudThicknessMeters);
        m_atmosphereReference->cloudLayer->SetDensity(m_settings.cloudDensity);
    }
}

SilverLiningSkyBox::~SilverLiningSkyBox() = default;
