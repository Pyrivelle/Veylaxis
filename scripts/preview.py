"""Optional QA plot. Run with QGIS's Python (GDAL, numpy and matplotlib).
Reconstruction and the OSG viewer do not depend on Python.
"""
import json
from pathlib import Path
import sys
import numpy as np
from osgeo import gdal, ogr
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap, BoundaryNorm

gdal.UseExceptions()
folder = Path(sys.argv[1] if len(sys.argv) > 1 else 'output/region001')
ds = gdal.Open(str(folder / 'region001_elevation.tif'))
z = ds.ReadAsArray()
valid = z != ds.GetRasterBand(1).GetNoDataValue()
z = np.ma.masked_where(~valid, z)
gt = ds.GetGeoTransform()
extent = [0, ds.RasterXSize * gt[1], 0, -ds.RasterYSize * gt[5]]
q = gdal.Open(str(folder / 'region001_quality.tif')).ReadAsArray()
fig, axes = plt.subplots(1, 2, figsize=(12.5, 6), constrained_layout=True)
image = axes[0].imshow(z, origin='upper', extent=extent, cmap='terrain', interpolation='nearest')
fig.colorbar(image, ax=axes[0], label='Elevation below ENC sounding datum (m)', shrink=.8)
axes[0].set_title('Region 001 | reconstructed seabed, 5 m grid')
vectors = ogr.Open(str(folder / 'region001_sources.gpkg'))

def draw_lines(geometry, ax):
    if geometry.GetGeometryType() in (ogr.wkbLineString, ogr.wkbLineString25D):
        points = np.array(geometry.GetPoints())
        ax.plot(points[:, 0] - gt[0], points[:, 1] - (gt[3] + ds.RasterYSize * gt[5]), color='white', lw=.8, alpha=.9)
    else:
        for child in geometry:
            draw_lines(child, ax)

for feature in vectors.GetLayerByName('DEPCNT_roi'):
    draw_lines(feature.GetGeometryRef(), axes[0])
for feature in vectors.GetLayerByName('SOUNDG_roi'):
    p = feature.GetGeometryRef()
    x, y = p.GetX()-gt[0], p.GetY()-(gt[3]+ds.RasterYSize*gt[5])
    axes[0].scatter([x], [y], c='red', edgecolors='white', s=45, zorder=4)
    axes[0].annotate(f" {feature.GetField('depth_min'):g} m sounding", (x, y), color='black', xytext=(6,-15), textcoords='offset points', fontsize=8)
cmap = ListedColormap(['#eeeeee', '#227f86', '#e9b44c', '#c45050'])
im = axes[1].imshow(q, origin='upper', extent=extent, cmap=cmap, norm=BoundaryNorm([-.5,.5,1.5,2.5,3.5],4), interpolation='nearest')
fig.colorbar(im, ax=axes[1], ticks=[0,1,2,3], label='0 outside | 1 two contours | 2 one contour | 3 conflict repair', shrink=.8)
axes[1].set_title('Reconstruction support | fallback is explicit')
for ax in axes:
    ax.set_xlabel('Easting from raster west edge (m)')
    ax.set_ylabel('Northing from raster south edge (m)')
    ax.set_aspect('equal')
fig.savefig(folder / 'region001_preview.png', dpi=160)
max_step = 0.0
for axis in (0, 1):
    difference = np.ma.diff(z, axis=axis)
    max_step = max(max_step, float(np.ma.max(np.ma.abs(difference))))
print(json.dumps({'valid_cells': int(valid.sum()), 'max_adjacent_cell_step_m': max_step, 'preview': str(folder / 'region001_preview.png')}, indent=2))
