// 【阅读导航】相机交互约束：Z 轴向上，避免轨迹球操作把海面坐标系滚转成倒置视角。
#pragma once
#include <osgGA/TrackballManipulator>

// World/local scene is Cartesian and Z-up. Orbit with yaw/pitch only: no roll.
// This constrains orientation, not altitude; underwater navigation remains valid.
class ZUpManipulator : public osgGA::TrackballManipulator
{
  public:
    ZUpManipulator()
    {
        setVerticalAxisFixed(true);
        setAllowThrow(false);
        setAnimationTime(0.0);
        setMinimumDistance(0.5, false);
    }

  protected:
    bool performMovementLeftMouseButton(double elapsed, double dx, double dy) override
    {
        const bool moved = osgGA::TrackballManipulator::performMovementLeftMouseButton(elapsed, dx, dy);
        removeRoll();
        return moved;
    }

    bool performMouseDeltaMovement(float dx, float dy) override
    {
        const bool moved = osgGA::TrackballManipulator::performMouseDeltaMovement(dx, dy);
        removeRoll();
        return moved;
    }

  private:
    void removeRoll()
    {
        // OSG's incremental quaternion correction can accumulate tiny roll
        // over hundreds of drags. Rebuild an orthonormal world-Z-up basis.
        osg::Vec3d eye, center, up;
        getTransformation(eye, center, up);
        const osg::Matrixd view = osg::Matrixd::lookAt(eye, center, osg::Vec3d(0, 0, 1));
        _rotation = osg::Matrixd::inverse(view).getRotate();
        _rotation /= _rotation.length();
    }
};
