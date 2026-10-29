#pragma once
#include "veylaxis/vgds/Types.hpp"
#include "veylaxis/vgds/IslandTerrain.hpp"
#include <osg/Node>

namespace veylaxis::vgds
{
// Renderer-private interface; never included by the VGDS Core public headers.
osg::ref_ptr<osg::Node> createLandNode(const TilePackage&);
osg::ref_ptr<osg::Node> createLandNode(const TilePackage&, const std::shared_ptr<IslandTerrain>&);
}
