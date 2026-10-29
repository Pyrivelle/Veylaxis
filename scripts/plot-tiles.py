"""QA visualization of independently generated tiles; use QGIS Python."""
from pathlib import Path
import json
import numpy as np
from osgeo import gdal
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

gdal.UseExceptions()
root = Path(__file__).resolve().parents[1]/'output/tile-test'
before = gdal.Open(str(root/'baseline/mosaic.tif')).ReadAsArray()
after = gdal.Open(str(root/'halo250_normals/mosaic.tif')).ReadAsArray()
vmin, vmax = float(np.min(after)),float(np.max(after))
difference = before-after
fig, axes = plt.subplots(1,3,figsize=(16,5.3),constrained_layout=True)
for ax,data,title in zip(axes[:2],[before,after],['Independent tiles | no compute halo','Independent tiles | 250 m halo + shared-edge normals']):
    im=ax.imshow(data,extent=[0,3000,0,3000],origin='upper',cmap='terrain',vmin=vmin,vmax=vmax,interpolation='nearest')
    ax.set_title(title,fontsize=10)
    for x in range(3):
        for y in range(3):
            ax.text(500+x*1000,500+y*1000,f'({x-1:+d},{y-1:+d})',ha='center',va='center',fontsize=8,bbox=dict(facecolor='white',alpha=.65,edgecolor='none'))
fig.colorbar(im,ax=list(axes[:2]),label='Elevation (m)',shrink=.8)
limit=max(float(np.max(np.abs(difference))),.001)
error=axes[2].imshow(difference,extent=[0,3000,0,3000],origin='upper',cmap='RdBu_r',vmin=-limit,vmax=limit,interpolation='nearest')
axes[2].set_title('No-halo mosaic minus corrected mosaic',fontsize=10)
fig.colorbar(error,ax=axes[2],label='Elevation difference (m)',shrink=.8)
for ax in axes:
    for value in [1000,2000]:
        ax.axvline(value,color='black',lw=.6,alpha=.65);ax.axhline(value,color='black',lw=.6,alpha=.65)
    ax.set_xlabel('Easting from test west edge (m)');ax.set_ylabel('Northing from test south edge (m)')
    ax.set_xticks([0,1000,2000,3000]);ax.set_yticks([0,1000,2000,3000])
fig.savefig(root/'comparison.png',dpi=170)
summary={}
for name in ['baseline','halo250','halo250_normals']:
    manifest=json.loads((root/name/'manifest.json').read_text())
    summary[name]={
        'heights':json.loads((root/name/'seams.json').read_text()),
        'model':json.loads((root/name/'model-seams.json').read_text()),
        'tile_generation_seconds':[t['seconds'] for t in manifest['tiles']],
        'model_export_seconds':sum(t.get('model_seconds',0) for t in manifest['tiles'])}
(root/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print('Saved',root/'comparison.png')
