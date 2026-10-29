"""Run nine independent C++ reconstructions and compare coincident edge samples.
Use QGIS Python (GDAL/numpy/matplotlib). No tile is cropped from the reference.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time
import numpy as np
from osgeo import gdal

gdal.UseExceptions()
ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'output/tile-test'
RES = 5.0
X0, Y0 = 503500.0, 4856500.0
OFFSETS = [(0, 0)] + [(x, y) for y in (1, 0, -1) for x in (-1, 0, 1) if (x, y) != (0, 0)]

def app_environment():
    cache = (ROOT / 'build/CMakeCache.txt').read_text(encoding='utf-8')
    installed = next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('VCPKG_INSTALLED_DIR:PATH='))
    prefix = Path(installed) / 'x64-windows'
    env = os.environ.copy()
    env.update(GDAL_DATA=str(prefix / 'share/gdal'), S57_CSV=str(prefix / 'share/gdal'), PROJ_DATA=str(prefix / 'share/proj'), GDAL_DRIVER_PATH='disable')
    env['OSG_LIBRARY_PATH'] = str(prefix / 'plugins/osgPlugins-3.6.5')
    env['PATH'] = str(prefix / 'bin') + os.pathsep + env.get('PATH', '')
    return env

def invoke(folder, bounds, halo):
    folder.mkdir(parents=True, exist_ok=True)
    args = [str(ROOT / 'build/Release/Veylaxis.exe'), 'reconstruct', '--input', str(ROOT / 'S57_DATA/ENC_ROOT/US2EC04M/US2EC04M.000'), '--output', str(folder), '--projected-bounds', *map(str, bounds), '--grid-nodes', '--compute-halo', str(halo)]
    start = time.perf_counter()
    result = subprocess.run(args, cwd=ROOT, env=app_environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    elapsed = time.perf_counter() - start
    (folder / 'generation.log').write_text(result.stdout, encoding='utf-8')
    if result.returncode not in (0, 2):
        raise RuntimeError(f'{folder.name}: {result.stdout}')
    return elapsed, result.returncode

def read(folder):
    ds = gdal.Open(str(folder / 'region001_elevation.tif'))
    z = ds.ReadAsArray()
    valid = np.isfinite(z) & (z != ds.GetRasterBand(1).GetNoDataValue())
    return z, valid, ds.GetGeoTransform()

def stats(values):
    values = np.asarray(values, dtype=float)
    return {'count': int(values.size), 'max': float(np.max(np.abs(values))) if values.size else None,
            'rmse': float(np.sqrt(np.mean(values * values))) if values.size else None}

def generate(name, halo):
    dest = BASE / name
    manifest = {'crs': 'EPSG:32619', 'origin': [504000, 4857000], 'tile_origin': [X0, Y0], 'tile_size_m': 1000, 'spacing_m': RES, 'compute_halo_m': halo, 'independent_reconstruction': True, 'tiles': []}
    for x, y in OFFSETS:
        folder = dest / f'x{x}_y{y}'
        bounds = [X0 + x * 1000, Y0 + y * 1000, X0 + (x + 1) * 1000, Y0 + (y + 1) * 1000]
        elapsed, code = invoke(folder, bounds, halo)
        metadata = json.loads((folder / 'region001_report.json').read_text())
        manifest['tiles'].append({'x': x, 'y': y, 'folder': folder.name, 'bounds': bounds, 'seconds': elapsed, 'exit_code': code, 'valid_nodes': metadata['valid_cells'], 'total_nodes': metadata['roi_cells']})
        print(f'{name} ({x:+d},{y:+d}): {elapsed:.3f}s; {metadata["valid_cells"]}/{metadata["roi_cells"]} nodes', flush=True)
    (dest / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')

def analyse(name):
    dest = BASE / name
    manifest = json.loads((dest / 'manifest.json').read_text())
    tiles = {(t['x'], t['y']): read(dest / t['folder']) for t in manifest['tiles']}
    reference, ref_valid, _ = read(BASE / 'reference')
    seams = []
    mosaic = np.full((601, 601), -9999, dtype=np.float32)
    occupied = np.zeros(mosaic.shape, dtype=bool)
    reference_deltas = []
    for (x, y), (z, valid, gt) in tiles.items():
        assert z.shape == (201, 201), z.shape
        assert abs(gt[0] + RES/2 - (X0+x*1000)) < 1e-7 and abs(gt[3] - RES/2 - (Y0+(y+1)*1000)) < 1e-7
        r, c = (1-y)*200, (x+1)*200
        window = mosaic[r:r+201, c:c+201]
        window_occupied = occupied[r:r+201, c:c+201]
        take = valid & ~window_occupied
        window[take] = z[take]
        window_occupied |= valid
        ref = reference[r:r+201, c:c+201]
        good = valid & ref_valid[r:r+201, c:c+201]
        reference_deltas.extend((z[good] - ref[good]).tolist())
        for dx, dy, direction in [(1, 0, 'east'), (0, 1, 'north')]:
            if (x+dx, y+dy) not in tiles:
                continue
            other, other_valid, _ = tiles[(x+dx, y+dy)]
            a, av = (z[:, -1], valid[:, -1]) if dx else (z[0, :], valid[0, :])
            b, bv = (other[:, 0], other_valid[:, 0]) if dx else (other[-1, :], other_valid[-1, :])
            good = av & bv
            delta = a[good] - b[good]
            seams.append({'tile': [x, y], 'neighbor': [x+dx, y+dy], 'direction': direction,
                          'height_m': stats(delta), 'validity_mismatch': int(np.count_nonzero(av != bv)),
                          'nodes_over_1mm': int(np.count_nonzero(np.abs(delta) > .001))})
    result = {'phase': name, 'independent_tiles': 9, 'seam_pairs': len(seams), 'height_tolerance_m': .001,
              'total_seconds': sum(t['seconds'] for t in manifest['tiles']),
              'max_seam_height_m': max(s['height_m']['max'] or 0 for s in seams),
              'seam_validity_mismatches': sum(s['validity_mismatch'] for s in seams),
              'shared_nodes_over_1mm': sum(s['nodes_over_1mm'] for s in seams),
              'valid_unique_nodes': int(occupied.sum()), 'total_unique_nodes': int(mosaic.size),
              'against_monolithic_reference_m': stats(reference_deltas), 'seams': seams}
    result['height_seams_pass'] = result['max_seam_height_m'] <= .001 and result['seam_validity_mismatches'] == 0
    driver = gdal.GetDriverByName('GTiff')
    ds = driver.Create(str(dest / 'mosaic.tif'), 601, 601, 1, gdal.GDT_Float32, options=['COMPRESS=DEFLATE','TILED=YES'])
    ds.SetProjection(gdal.Open(str(dest / manifest['tiles'][0]['folder'] / 'region001_elevation.tif')).GetProjection())
    ds.SetGeoTransform([X0-1000-RES/2, RES, 0, Y0+2000+RES/2, 0, -RES]);ds.SetMetadataItem('AREA_OR_POINT', 'Point')
    ds.GetRasterBand(1).SetNoDataValue(-9999);ds.GetRasterBand(1).WriteArray(mosaic);ds = None
    (dest / 'seams.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k != 'seams'}, indent=2), flush=True)
    return result

def models(name):
    dest = BASE / name
    manifest = json.loads((dest / 'manifest.json').read_text())
    for tile in manifest['tiles']:
        folder = dest / tile['folder']
        start = time.perf_counter()
        result = subprocess.run([str(ROOT/'build/Release/Veylaxis.exe'), 'view', str(folder/'region001_elevation.tif'), '--export', str(folder/'region001_terrain.osgb'), '--color-range', '-50', '0', '--normal-mode', 'local' if name=='halo250' else 'cached'], cwd=ROOT, env=app_environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
        tile['model_seconds'] = time.perf_counter()-start
        (folder/'model.log').write_text(result.stdout, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(result.stdout)
    (dest/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    result = subprocess.run([str(ROOT/'build/Release/Test.exe'), '--tile-scene', str(dest/'manifest.json'), str(dest/'tiles.osgb')], cwd=ROOT, env=app_environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    (dest/'assembly.log').write_text(result.stdout, encoding='utf-8')
    print(result.stdout, flush=True)
    if result.returncode not in (0,2):
        raise RuntimeError('Tile scene assembly failed')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--phase', default='baseline')
    parser.add_argument('--halo', type=float, default=0)
    parser.add_argument('--analyse-only', action='store_true')
    parser.add_argument('--models', action='store_true')
    args = parser.parse_args()
    if not args.analyse_only:
        generate(args.phase, args.halo)
        if args.phase == 'baseline' or not (BASE / 'reference/region001_elevation.tif').exists():
            elapsed, code = invoke(BASE / 'reference', [X0-1000,Y0-1000,X0+2000,Y0+2000], 250)
            print(f'Monolithic reference only: {elapsed:.3f}s, exit={code}', flush=True)
    analyse(args.phase)
    if args.models:
        models(args.phase)
        if args.phase == 'halo250_normals':
            heights = json.loads((BASE/args.phase/'seams.json').read_text())
            geometry = json.loads((BASE/args.phase/'model-seams.json').read_text())
            if not heights['height_seams_pass'] or not geometry['geometry_and_shading_pass']:
                raise RuntimeError('Final independent tile seam validation failed')
