#!/usr/bin/env python3
"""把公开瓦片底图拼成与 DEM 对齐的等距圆柱纹理。

输出：RGB uint8 raw，2048x2048，行序自北向南（与 HeightField 一致）。
着色器采样时需要把 v 翻过来（1-v），因为 OpenGL 纹理原点在左下。

用法：
    python build_albedo.py --service World_Imagery --out ../data/terrain_albedo_rgb.raw
数据源：Esri World Imagery / World Shaded Relief（公开瓦片服务，仅作本地演示底图）
"""

import argparse
import math
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

TILE_URL = ("https://server.arcgisonline.com/ArcGIS/rest/services/{svc}/MapServer/tile/"
            "{z}/{y}/{x}")
TILE_SIZE = 256


def lonlat_to_tile(lon, lat, z):
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    lat_rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n
    return x, y


def download(args):
    svc, z, x, y, cache = args
    path = os.path.join(cache, f"{svc}_{z}_{x}_{y}.png")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path, True
    url = TILE_URL.format(svc=svc, z=z, x=x, y=y)
    tmp = path + ".part"
    proc = subprocess.run(["curl.exe", "-s", "--max-time", "40", "-o", tmp, url],
                          capture_output=True)
    if proc.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) < 100:
        if os.path.exists(tmp):
            os.remove(tmp)
        return path, False
    os.replace(tmp, path)
    return path, True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--service", default="World_Imagery")
    ap.add_argument("--lon-min", type=float, default=111.0)
    ap.add_argument("--lon-max", type=float, default=116.0)
    ap.add_argument("--lat-min", type=float, default=27.4)
    ap.add_argument("--lat-max", type=float, default=30.4)
    ap.add_argument("--zoom", type=int, default=10)
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    ".albedo_cache"))
    args = ap.parse_args()

    z = args.zoom
    n_tiles = 2 ** z

    tx0, ty1 = lonlat_to_tile(args.lon_min, args.lat_min, z)
    tx1, ty0 = lonlat_to_tile(args.lon_max, args.lat_max, z)
    tx0, tx1 = int(math.floor(tx0)), int(math.floor(tx1))
    ty0, ty1 = int(math.floor(ty0)), int(math.floor(ty1))
    cols = tx1 - tx0 + 1
    rows = ty1 - ty0 + 1
    print(f"[albedo] {args.service} zoom={z} 瓦片 {cols}x{rows} = {cols * rows} 张")

    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    tasks = [(args.service, z, x, y, args.cache)
             for y in range(ty0, ty1 + 1) for x in range(tx0, tx1 + 1)]
    failed = []
    with ThreadPoolExecutor(max_workers=8) as pool:
        for path, ok in pool.map(download, tasks):
            if not ok:
                failed.append(path)
    if failed:
        print(f"[albedo] 下载失败 {len(failed)} 张，例如 {failed[0]}", file=sys.stderr)
        return 1

    mosaic = np.zeros((rows * TILE_SIZE, cols * TILE_SIZE, 3), dtype=np.uint8)
    for y in range(ty0, ty1 + 1):
        for x in range(tx0, tx1 + 1):
            path = os.path.join(args.cache, f"{args.service}_{z}_{x}_{y}.png")
            tile = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
            r = (y - ty0) * TILE_SIZE
            c = (x - tx0) * TILE_SIZE
            mosaic[r:r + TILE_SIZE, c:c + TILE_SIZE, :] = tile[:, :, :3]

    size = args.size
    out = np.zeros((size, size, 3), dtype=np.uint8)

    lons = args.lon_min + (np.arange(size, dtype=np.float64) + 0.5) / size * (args.lon_max - args.lon_min)
    lats = args.lat_max - (np.arange(size, dtype=np.float64) + 0.5) / size * (args.lat_max - args.lat_min)

    gx = (lons + 180.0) / 360.0 * n_tiles * TILE_SIZE - tx0 * TILE_SIZE
    lat_rad = np.radians(lats)
    gy = ((1.0 - np.log(np.tan(lat_rad) + 1.0 / np.cos(lat_rad)) / np.pi) / 2.0 * n_tiles
          * TILE_SIZE) - ty0 * TILE_SIZE

    x0 = np.clip(np.floor(gx).astype(np.int64), 0, mosaic.shape[1] - 2)
    y0 = np.clip(np.floor(gy).astype(np.int64), 0, mosaic.shape[0] - 2)
    fx = (gx - x0).astype(np.float32)
    fy = (gy - y0).astype(np.float32)

    for ch in range(3):
        plane = mosaic[:, :, ch].astype(np.float32)
        for j in range(size):
            r0 = y0[j]
            r1 = r0 + 1
            wy = fy[j]
            top = plane[r0, x0] * (1.0 - fx) + plane[r0, x0 + 1] * fx
            bot = plane[r1, x0] * (1.0 - fx) + plane[r1, x0 + 1] * fx
            out[j, :, ch] = (top * (1.0 - wy) + bot * wy + 0.5).astype(np.uint8)

    out.tofile(args.out)
    print(f"[albedo] 已写出 {args.out}  {size}x{size} RGB uint8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
