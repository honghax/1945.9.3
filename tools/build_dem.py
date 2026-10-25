#!/usr/bin/env python3
"""把 AWS Terrain Tiles（terrarium 编码）拼成指定经纬度范围的高程栅格。

输出：单通道 float32 小端 raw，行序自北向南，与 war3d 的 HeightField 约定一致。

用法示例（第一次长沙会战范围）：
    python build_dem.py --lon-min 111 --lon-max 116 --lat-min 27.4 --lat-max 30.4 \
        --zoom 10 --size 1024 --out ../data/terrain_hunan_float32.raw

数据源：https://registry.opendata.aws/terrain-tiles/
terrarium 编码：height = (R * 256 + G + B / 256) - 32768（米）
"""

import argparse
import math
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

TILE_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
TILE_SIZE = 256


def lonlat_to_tile(lon, lat, z):
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    lat_rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n
    return x, y


def download_tile(z, x, y, cache_dir):
    path = os.path.join(cache_dir, f"{z}_{x}_{y}.png")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path, True
    url = TILE_URL.format(z=z, x=x, y=y)
    tmp = path + ".part"
    proc = subprocess.run(
        ["curl.exe", "-s", "--max-time", "40", "-o", tmp, url],
        capture_output=True,
    )
    if proc.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) < 100:
        if os.path.exists(tmp):
            os.remove(tmp)
        return path, False
    os.replace(tmp, path)
    return path, True


def decode_terrarium(img):
    arr = np.asarray(img.convert("RGB"), dtype=np.float32)
    return arr[:, :, 0] * 256.0 + arr[:, :, 1] + arr[:, :, 2] / 256.0 - 32768.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lon-min", type=float, default=111.0)
    ap.add_argument("--lon-max", type=float, default=116.0)
    ap.add_argument("--lat-min", type=float, default=27.4)
    ap.add_argument("--lat-max", type=float, default=30.4)
    ap.add_argument("--zoom", type=int, default=10)
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".tile_cache"))
    args = ap.parse_args()

    z = args.zoom
    n_tiles = 2 ** z

    tx0, ty1 = lonlat_to_tile(args.lon_min, args.lat_min, z)   # 西南角
    tx1, ty0 = lonlat_to_tile(args.lon_max, args.lat_max, z)   # 东北角
    tx0, tx1 = int(math.floor(tx0)), int(math.floor(tx1))
    ty0, ty1 = int(math.floor(ty0)), int(math.floor(ty1))

    cols = tx1 - tx0 + 1
    rows = ty1 - ty0 + 1
    print(f"[dem] zoom={z} 瓦片范围 x={tx0}..{tx1} y={ty0}..{ty1} → {cols}x{rows} = {cols * rows} 张")

    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    tasks = [(z, x, y, args.cache) for y in range(ty0, ty1 + 1) for x in range(tx0, tx1 + 1)]
    failed = []
    with ThreadPoolExecutor(max_workers=8) as pool:
        for path, ok in pool.map(lambda t: download_tile(*t), tasks):
            if not ok:
                failed.append(path)
    if failed:
        print(f"[dem] 下载失败 {len(failed)} 张，例如 {failed[0]}", file=sys.stderr)
        return 1

    mosaic = np.zeros((rows * TILE_SIZE, cols * TILE_SIZE), dtype=np.float32)
    for y in range(ty0, ty1 + 1):
        for x in range(tx0, tx1 + 1):
            path = os.path.join(args.cache, f"{z}_{x}_{y}.png")
            tile = decode_terrarium(Image.open(path))
            if tile.shape[0] != TILE_SIZE or tile.shape[1] != TILE_SIZE:
                print(f"[dem] 瓦片尺寸异常：{path} {tile.shape}", file=sys.stderr)
                return 1
            r = (y - ty0) * TILE_SIZE
            c = (x - tx0) * TILE_SIZE
            mosaic[r:r + TILE_SIZE, c:c + TILE_SIZE] = tile

    size = args.size
    out = np.zeros((size, size), dtype=np.float32)

    # 目标像元的经纬度 → 全局像素坐标 → 拼接图局部坐标，双线性采样
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

    for j in range(size):
        r0 = y0[j]
        r1 = r0 + 1
        wy = fy[j]
        top = mosaic[r0, x0] * (1.0 - fx) + mosaic[r0, x0 + 1] * fx
        bot = mosaic[r1, x0] * (1.0 - fx) + mosaic[r1, x0 + 1] * fx
        out[j, :] = top * (1.0 - wy) + bot * wy

    # 水域在 terrarium 里可能出现极深的负值（湖底测深/空洞），统一压到浅水
    out = np.clip(out, -30.0, 5000.0)

    out.tofile(args.out)
    print(f"[dem] 已写出 {args.out}  {size}x{size} float32")
    print(f"[dem] 高程范围 {float(out.min()):.1f} ~ {float(out.max()):.1f} m，均值 {float(out.mean()):.1f} m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
