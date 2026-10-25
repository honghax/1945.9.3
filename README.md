# war3d

A work-in-progress demo of an interactive 3D map of the Chinese People's War of Resistance Against Japan.

This project is being made to commemorate the 81st anniversary of the Chinese People's Victory in the War of Resistance Against Japan (1945–2026).

**Status: early development.** Only the First Battle of Changsha (September–October 1939) is implemented so far — it serves as a testbed for the terrain, rendering and data pipeline. The eventual goal is a continuous 1931–1945 timeline.

## What works today

- Real terrain from AWS Terrain Tiles (1024² DEM covering roughly 487 × 333 km of northern Hunan, Hubei and Jiangxi)
- Rivers and Dongting Lake rendered as water — depth gradient, Fresnel reflection, ripples, shoreline foam — with river valleys carved into the DEM
- Unit movement arrows that grow along the timeline, occupied areas, defensive lines
- Event markers, artillery flashes, narration subtitles
- Timeline scrubbing, playback speed, click a place to move the camera

## Build

Windows, MSVC + Ninja. All dependencies are vendored under `third_party/`; no external paths are used.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

Output is `build\war3d.exe`. OpenGL 4.3 required.

## Data

Historical events, sources and casualty figures live in `data/*.csv`. Every event carries a source ID and a confidence level, and the conflicting Chinese and Japanese casualty figures are listed side by side rather than merged into one number.

Two caveats worth knowing:

- River courses are interpolated from verified place-name coordinates, not surveyed geometry. Shapes are close, but not authoritative.
- Occupied areas and defensive lines are schematic, traced from textual descriptions in the sources.

This is a demo, not an academic reference.
