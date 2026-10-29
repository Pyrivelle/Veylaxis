#pragma once
#include "veylaxis/vgds/S57.hpp"
#include <ogrsf_frmts.h>

namespace veylaxis::vgds
{
TerrainSource readBathymetry(GDALDataset&, const OGRSpatialReference&, S57Report&);
}
