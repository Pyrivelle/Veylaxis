#pragma once

// Central render-bin assignments. Lower values render earlier.
// Keep these values unique so render ordering can be inspected and adjusted
// without searching individual rendering modules.
#define FARSEA_RENDER_BIN_MODELS 1
#define FARSEA_RENDER_BIN_WATER_DEPTH_MASK 9
#define FARSEA_RENDER_BIN_OCEAN 10
#define FARSEA_RENDER_BIN_SKY 98
#define FARSEA_RENDER_BIN_CLOUDS 99
#define FARSEA_RENDER_BIN_ANNOTATION_OVERLAY 9999

#define FARSEA_RENDER_BIN_NAME "RenderBin"
