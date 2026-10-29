"""Read-only navigation audit using the installed GDAL C API (no Python GIS packages)."""
import ctypes as C
import json, os, pathlib, math, subprocess

root = pathlib.Path(__file__).resolve().parent.parent
prefix = pathlib.Path(os.environ.get("VCPKG_ROOT", "C:/CProj/vcpkg"))/"installed/x64-windows"
dll_dir = os.add_dll_directory(str(prefix/"bin"))
os.environ.update(GDAL_DATA=str(prefix/"share/gdal"), S57_CSV=str(prefix/"share/gdal"),
                  PROJ_DATA=str(prefix/"share/proj"), GDAL_DRIVER_PATH="disable")
os.environ["PATH"] = str(prefix/"bin")+os.pathsep+os.environ["PATH"]
g = C.CDLL(str(prefix/"bin/gdal.dll"))
P, I, D, S = C.c_void_p, C.c_int, C.c_double, C.c_char_p
def fn(name, result, *args):
    f=getattr(g,name); f.restype=result; f.argtypes=list(args); return f
fn("GDALAllRegister",None)()
open_ds=fn("GDALOpenEx",P,S,I,P,P,P); close=fn("GDALClose",None,P)
layer=fn("GDALDatasetGetLayerByName",P,P,S); next_f=fn("OGR_L_GetNextFeature",P,P)
destroy=fn("OGR_F_Destroy",None,P); geom=fn("OGR_F_GetGeometryRef",P,P)
gt=fn("OGR_G_GetGeometryType",I,P); gx=fn("OGR_G_GetX",D,P,I); gy=fn("OGR_G_GetY",D,P,I)
field_index=fn("OGR_F_GetFieldIndex",I,P,S); field_str=fn("OGR_F_GetFieldAsString",S,P,I)
def field(f,key):
    n=field_index(f,key.encode()); return field_str(f,n).decode() if n>=0 else None
new_srs=fn("OSRNewSpatialReference",P,S); set_srs=fn("OSRSetFromUserInput",I,P,S)
axis=fn("OSRSetAxisMappingStrategy",None,P,I)
new_ct=fn("OCTNewCoordinateTransformation",P,P,P)
transform=fn("OCTTransform",I,P,I,C.POINTER(D),C.POINTER(D),P)
setup=json.loads((root/"config/vgds-catalog.json").read_text(encoding="utf-8-sig"))
models=json.loads((root/"config/navigation_models.json").read_text(encoding="utf-8-sig"))
src=new_srs(None); dst=new_srs(None); set_srs(src,b"EPSG:4326"); set_srs(dst,setup["grid"]["crs"].encode());axis(src,0);axis(dst,0)
ct=new_ct(src,dst)
assert ct
start=setup["start"]; origin=setup["grid"]["origin"]; size=setup["grid"]["size"]
center=[math.floor((start[i]-origin[i])/size) for i in range(2)]
active={(center[0]+x,center[1]+y) for x in (-1,0,1) for y in (-1,0,1)}
classes=["BOYLAT","BOYCAR","BOYISD","BOYSAW","BOYSPP","LITVES","MORFAC","BERTHS","LIGHTS","BCNLAT","BCNCAR","BCNISD","BCNSAW","BCNSPP"]
records=[]
for file in sorted((root/setup["source"]["root"]).rglob("*.000")):
    ds=open_ds(str(file).encode(),4,None,None,None)
    if not ds: raise RuntimeError(str(file))
    for kind in classes:
        lyr=layer(ds,kind.encode())
        if not lyr: continue
        while f:=next_f(lyr):
            geometry=geom(f)
            if geometry and (gt(geometry)&0x7fffffff)%1000==1:
                lon,lat=gx(geometry,0),gy(geometry,0); x,y=D(lon),D(lat)
                assert transform(ct,1,C.byref(x),C.byref(y),None)
                tile=[math.floor((v-o)/size) for v,o in zip((x.value,y.value),origin)]
                attrs={k:field(f,k) for k in ("LNAM","CATLAM","CATCAM","CATMOR","CATSPM")}
                match=[r["id"] for r in models["mappings"] if r["objectClass"]==kind and ("attribute" not in r or attrs[r["attribute"]]==str(r["value"])) and r["modelPath"]]
                records.append(dict(cell=file.stem,objectClass=kind,lon=lon,lat=lat,x=x.value,y=y.value,tile=tile,
                                    distance_m=math.hypot(x.value-start[0],y.value-start[1]),attributes=attrs,mappings=match))
            destroy(f)
    close(ds)
records.sort(key=lambda r:r["distance_m"])
raw_active=[r for r in records if tuple(r["tile"]) in active]
nearest_mapped=[r for r in records if r["mappings"]][:20]
audit_dir=root/"output/vgds";audit_dir.mkdir(parents=True,exist_ok=True)
cfg=json.loads(json.dumps(setup));cfg["osgb"]=False;cfg["source"]["bathymetry"]=False;cfg["island_terrain"]={"enabled":False};cfg["cache"]="output/vgds-cache/navigation-audit"
cfg_path=audit_dir/"navigation-audit-config.json";cfg_path.write_text(json.dumps(cfg),encoding="utf-8")
tiles=sorted(active|{tuple(r["tile"]) for r in nearest_mapped})
requests="\n".join(json.dumps(dict(schema=1,requestId=f"audit-{x}-{y}",tile=dict(gridId=setup["grid"]["id"],x=x,y=y),layers=["navigation_aids"],terrainSpacingMeters=10)) for x,y in tiles)+"\n"
process=subprocess.run([str(root/"build/Release/VgdsService.exe"),"--config",str(cfg_path)],input=requests,text=True,cwd=root,capture_output=True,check=True)
packages=[json.loads(line) for line in process.stdout.splitlines()]
errors=[p for p in packages if "error" in p];assert not errors,errors
summary=[]
for p in packages:
    summary.append(dict(tile=p["tile"],active=(p["tile"]["x"],p["tile"]["y"]) in active,layers=p["layers"],features=p["features"]))
report=dict(start=start,center_tile=center,active_bounds=[origin[0]+(center[0]-1)*size,origin[1]+(center[1]-1)*size,origin[0]+(center[0]+2)*size,origin[1]+(center[1]+2)*size],
            raw_point_count=len(records),raw_active=raw_active,nearest_raw=records[:20],nearest_mapped=nearest_mapped,catalog_tiles=summary,service_log=process.stderr)
(audit_dir/"navigation-visibility-audit.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
print(json.dumps(dict(raw_active=len(raw_active),catalog_active=sum(len(p["features"]) for p in summary if p["active"]),nearest_raw=records[:3],nearest_mapped=nearest_mapped[:3]),ensure_ascii=False,indent=2))
