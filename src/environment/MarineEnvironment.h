#pragma once
#include "SilverLiningSkyBox.h"
#include "TritonOceanSurface.h"
#include <osg/observer_ptr>

// Owns only runtime SDK nodes; seabed exports remain ordinary OSG geometry.
class MarineEnvironment
{
  public:
    MarineEnvironment(osgViewer::Viewer* viewer, osg::Group* root, double longitude, double latitude);
    void updateLighting();
    bool ready() const;
    bool failed() const;
    void toggleOcean();
    void shutdown();

  private:
    osg::observer_ptr<osgViewer::Viewer> viewer_;
    osg::observer_ptr<osg::Group> root_;
    osg::ref_ptr<SilverLiningSkyBox> sky_;
    osg::ref_ptr<TritonOceanSurface> ocean_;
};
