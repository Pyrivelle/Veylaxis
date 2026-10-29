# Veylaxis 三维 Viewer

当前默认显示 SilverLining 天空、Triton 海洋和区域 001 海底，XYZ 单位为米，Z 缩放已恢复为 **1 倍**。

```powershell
.\scripts\view-region001.ps1
```

F1 为海面视角，F2 为水下视角，O 切换海面显示；左键旋转、中键平移、滚轮缩放、空格复位。

相机已固定世界 Z 向上，拖动不会产生横滚。相机高度仍可变化。海底 Z 为约 -36.85 至 -1.80 m，海面零位为 0 m，所以海底在水下符合水深数据；没有额外竖直平移。

独立检查海底并放大高程可运行：

```powershell
.\scripts\view-region001.ps1 -SeabedOnly -ZScale 10
```

导出海底模型保持真实比例：

```powershell
.\scripts\view-region001.ps1 -ExportOnly
```

完整的依赖、资源部署、坐标关系、渲染顺序、故障修复和测试方法见 [海洋场景融合说明](marine-integration.md)。
