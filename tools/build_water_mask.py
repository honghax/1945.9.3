#!/usr/bin/env python3
"""从 ESRI World_Topo_Map 瓦片提取水系轮廓掩码。

制图底图把水体统一画成浅蓝（约 204,237,255），用颜色判据就能干净分离；
卫星影像做不到——洞庭湖在影像里是含沙的黄绿色，与农田几乎同色。

输出：
    data/water_mask_u8.raw      2048x2048 uint8，0/255，行序自北向南
    assets/water_mask_preview.png  检查用

用法：
    python build_water_mask.py
"""

import argparse
import math
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

TILE_SIZE = 256
URL = ("https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/"
       "tile/{z}/{y}/{x}")

# 覆盖顺序：先低分辨率铺满全域，再用高分辨率区域覆盖细节
# 注意：z=12 区域覆盖时曾把整幅掩码重置成该区域内容，导致水系位置整体偏移；
# 在修好按 bbox 局部替换之前，只用 z=11 全域，位置是正确的
REGIONS = [
    (11, 111.0, 116.0, 27.4, 30.4),
]


def lonlat_to_tile(lon, lat, z):
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    r = math.radians(lat)
    y = (1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * n
    return x, y


def download(job):
    z, x, y, cache = job
    path = os.path.join(cache, f"topo_{z}_{x}_{y}.png")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path, True
    tmp = path + ".part"
    proc = subprocess.run(
        ["curl.exe", "-s", "--max-time", "40", "-o", tmp, URL.format(z=z, y=y, x=x)],
        capture_output=True)
    if proc.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) < 100:
        if os.path.exists(tmp):
            os.remove(tmp)
        return path, False
    os.replace(tmp, path)
    return path, True


def extract_water(rgb):
    """制图底图的水色：r<g<b 且蓝分量明显偏高。白底与绿地都会被排除。"""
    r = rgb[:, :, 0].astype(np.int16)
    g = rgb[:, :, 1].astype(np.int16)
    b = rgb[:, :, 2].astype(np.int16)
    return (b > r + 25) & (g > r + 5) & (b > 150) & (r < 240)


def erode(m):
    p = np.pad(m, 1, constant_values=False)
    out = np.ones_like(m, dtype=bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out &= p[1 + dy:1 + dy + m.shape[0], 1 + dx:1 + dx + m.shape[1]]
    return out


def dilate(m):
    p = np.pad(m, 1, constant_values=False)
    out = np.zeros_like(m, dtype=bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out |= p[1 + dy:1 + dy + m.shape[0], 1 + dx:1 + dx + m.shape[1]]
    return out


def project_nearest(mask, z, tx0, ty0, mosaic_w, mosaic_h,
                    lon_min, lon_max, lat_min, lat_max, size):
    """把瓦片像素空间的掩码重投影到等距圆柱网格（最近邻）。"""
    n = 2 ** z
    lons = lon_min + (np.arange(size) + 0.5) / size * (lon_max - lon_min)
    lats = lat_max - (np.arange(size) + 0.5) / size * (lat_max - lat_min)
    gx = (lons + 180.0) / 360.0 * n * TILE_SIZE - tx0 * TILE_SIZE
    lat_r = np.radians(lats)
    gy = ((1.0 - np.log(np.tan(lat_r) + 1.0 / np.cos(lat_r)) / np.pi) / 2.0 * n
          * TILE_SIZE) - ty0 * TILE_SIZE

    ix = np.clip(np.round(gx).astype(np.int64), 0, mosaic_w - 1)
    iy = np.clip(np.round(gy).astype(np.int64), 0, mosaic_h - 1)

    out = np.zeros((size, size), dtype=bool)
    for j in range(size):
        out[j, :] = mask[iy[j], ix]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "..", "data", "water_mask_u8.raw"))
    ap.add_argument("--preview", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                      "..", "assets", "water_mask_preview.png"))
    ap.add_argument("--cache", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    ".topo_cache"))
    args = ap.parse_args()

    size = args.size
    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.preview)), exist_ok=True)

    result = np.zeros((size, size), dtype=bool)

    for z, lon_min, lon_max, lat_min, lat_max in REGIONS:
        tx0f, ty1f = lonlat_to_tile(lon_min, lat_min, z)
        tx1f, ty0f = lonlat_to_tile(lon_max, lat_max, z)
        tx0, tx1 = int(math.floor(tx0f)), int(math.floor(tx1f))
        ty0, ty1 = int(math.floor(ty0f)), int(math.floor(ty1f))
        cols, rows = tx1 - tx0 + 1, ty1 - ty0 + 1
        print(f"[mask] z={z} 瓦片 {cols}x{rows} = {cols * rows} 张")

        jobs = [(z, x, y, args.cache) for y in range(ty0, ty1 + 1) for x in range(tx0, tx1 + 1)]
        failed = 0
        with ThreadPoolExecutor(max_workers=8) as pool:
            for path, ok in pool.map(download, jobs):
                if not ok:
                    failed += 1
        if failed:
            print(f"[mask] 下载失败 {failed} 张", file=sys.stderr)
            if failed > len(jobs) // 4:
                return 1

        mosaic = np.zeros((rows * TILE_SIZE, cols * TILE_SIZE, 3), dtype=np.uint8)
        for y in range(ty0, ty1 + 1):
            for x in range(tx0, tx1 + 1):
                path = os.path.join(args.cache, f"topo_{z}_{x}_{y}.png")
                if not os.path.exists(path):
                    continue
                tile = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
                r = (y - ty0) * TILE_SIZE
                c = (x - tx0) * TILE_SIZE
                mosaic[r:r + TILE_SIZE, c:c + TILE_SIZE, :] = tile

        raw = extract_water(mosaic)
        print(f"[mask] z={z} 原始水像素 {raw.mean() * 100:.2f}%")

        projected = project_nearest(raw, z, tx0, ty0, mosaic.shape[1], mosaic.shape[0],
                                    lon_min, lon_max, lat_min, lat_max, size)
        result = projected

    # 制图底图里河流只有 1 像素宽，直接当水面会是发丝一样的细线。
    # 先用闭运算填掉湿地图案造成的椒盐斑点，再膨胀一遍让河道有可辨识的宽度；
    # 残余碎斑由后续按面积过滤清除
    for _ in range(2):
        result = dilate(result)
    for _ in range(2):
        result = erode(result)
    result = dilate(result)
    result = dilate(result)
    print(f"[mask] 加粗后水像素 {result.mean() * 100:.2f}%")

    data = (result.astype(np.uint8) * 255)
    data.tofile(args.out)
    print(f"[mask] 已写出 {args.out}  {size}x{size}")

    # 到岸距离（腐蚀层数），作为水深的近似：湖心深、窄河浅。
    # 没有 scipy 的 distance_transform，用逐层腐蚀逼近，每层 1 像素
    depth = np.zeros(result.shape, dtype=np.uint8)
    cur = result.copy()
    for i in range(1, 49):
        cur = erode(cur)
        if not cur.any():
            break
        depth[cur] = i
    depthPath = args.out.replace("water_mask_u8", "water_depth_u8")
    depth.tofile(depthPath)
    print(f"[mask] 水深场已写出 {depthPath}  最深层数 {depth.max()}")

    preview = np.full((size, size, 3), 74, dtype=np.uint8)
    preview[result] = (92, 162, 208)
    Image.fromarray(preview).resize((1024, 1024), Image.NEAREST).save(args.preview)
    print(f"[mask] 检查图 {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
