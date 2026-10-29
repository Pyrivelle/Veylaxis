#include "CameraNavigation.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
class NavigationProbe : public ZUpManipulator
{
  public:
    void orbit(double x, double y)
    {
        performMovementLeftMouseButton(0.016, x, y);
    }
};
}
int runCameraCheck()
try
{
    osg::ref_ptr<NavigationProbe> camera = new NavigationProbe;
    camera->setTransformation(osg::Vec3d(350, -700, 60), osg::Vec3d(0, 300, -5), osg::Vec3d(0, 0, 1));
    for (int i = 0; i < 400; ++i)
    {
        camera->orbit(0.08 * std::cos(i * 0.17), 0.15 * std::sin(i * 0.11));
        osg::Vec3d eye, center, up;
        camera->getTransformation(eye, center, up);
        osg::Vec3d forward = center - eye;
        forward.normalize();
        up.normalize();
        osg::Vec3d right = forward ^ up;
        right.normalize();
        // OSG's fixed-axis helper accepts float drag deltas. Allow sub-arcsecond
        // numerical error rather than requiring bit-exact orthogonality.
        if (std::abs(right.z()) > 1e-6 || up.z() <= 0)
        {
            std::cerr << "Orbit " << i << ": right.z=" << right.z() << " up.z=" << up.z() << '\n';
            throw std::runtime_error("Camera rolled or flipped away from world Z-up");
        }
    }
    // An altitude lock would prevent this; orientation locking must not.
    camera->setTransformation(osg::Vec3d(-300, -250, -12), osg::Vec3d(-20, 250, -20), osg::Vec3d(0, 0, 1));
    osg::Vec3d eye, center, up;
    camera->getTransformation(eye, center, up);
    if (std::abs(eye.z() + 12) > 1e-8 || up.z() <= 0)
        throw std::runtime_error("Underwater camera was shifted vertically");
    std::cout
        << "Camera: 400 orbit movements preserve world Z-up without roll; underwater eye Z=-12 m preserved\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
