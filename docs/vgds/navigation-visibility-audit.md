# 默认启动海区航标可见性核查

2026-10-29，检查当前 `config/vgds-catalog.json` 的默认启动点 EPSG:32619 (498700,4860750)，未修改默认启动设置。

## 结果

- 中心 Tile 为 (-5,4)，活动 3×3 为 X=-6…-4、Y=3…5，边界 (497500,4859500)–(500500,4862500)，共 9 km²。
- 直接通过 GDAL C API 读取本地 48 幅 ENC，扫描当前提取的 14 类导航点，共 1058 条记录；活动范围内为 0 条。
- 使用独立缓存，关闭 OSGB 和海底采样，仅请求 navigation_aids，重新通过真实 CatalogProvider 查询活动九块，返回同样为 0 条。原 Viewer 全图层缓存也为空。不是仅根据截图判断无物标。
- 最近导航点为 LIGHTS，约 8.517 km；该类别没有默认模型映射。
- 最近已有模型配置的对象来自 US2EC04M，为 BOYLAT、CATLAM=2（右侧标），来源 ID `US2EC04M:BOYLAT:022606D1B56E1170`。位置经纬度 (-68.832463,43.840134)，EPSG:32619 (513468.144598,4854130.764587)，原网格 Tile (9,-3)，距默认启动点直线约 16.184 km。
- 对上述真实 Tile 的 Catalog 请求返回了该 BOYLAT 和同位置的 LIGHTS；右侧标匹配 starboard 规则，路径为 `D:/Dragon/_Data/03/030013/030013.dmu3D`。LIGHTS 不额外造模型，符合当前映射范围。

结论：默认活动范围在本地 ENC 中没有这些导航点数据，因此没有航标实例可显示，不能据此认定 DMU 加载故障；也不能据此断言现实海域没有导航设施或本地小比例尺 ENC 已包含所有设施。

## 显示复核

`showTerrain()` 的海洋自动截图使用固定的场景局部相机 `(350,-700,60) → (0,300,-5)`，并没有自动瞄准航标或运动船只。因此把经纬度起点换到一个含航标 Tile，也可能因 Tile 内局部坐标不同而仍拍不到。普通海面预设同样应注意取景范围，数据已加载不等于处于当前相机视锥内。

另外生成了 `output/vgds/navigation-preview.json`，以真实右侧标附近为起点，配置专用网格原点，令船只、航标落入固定截图相机的取景范围；只用于核查，关闭海底采样及程序化岛屿，未替换主配置。

```powershell
.\scripts\view-vgds.ps1 -Config output/vgds/navigation-preview.json
```

可复核的完整数据在 `output/vgds/navigation-visibility-audit.json`，诊断脚本为 `scripts/audit-navigation.py`。脚本使用已有 GDAL DLL，无新依赖，不修改 ENC 或业务源代码，仅写诊断配置、报告和独立缓存。
