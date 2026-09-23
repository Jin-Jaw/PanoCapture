# PanoCapture: review handoff

**For:** a code review of the PanoCapture plugin and its web viewer.
**State (2026-09-23):** it works end to end on the test level `NewMap` (38 points). It hasn't been tried on a real interior, and **the project has no version control**, so review against the files as they are.

## What it is

An Unreal 5.6 plugin that renders Matterport-style virtual tours straight from a level, plus a three.js viewer for them. It's aimed at real-estate configurators. Engine: `G:\UE_5.6`. Project: `E:\Unreal\GDNLoadingTimeTest`.

**Unreal side** (`Source/PanoCapture`, one Runtime module):

| File | Role |
|---|---|
| `PanoCaptureCamera.h/.cpp` (1.8k lines) | The capture actor. Six `USceneCaptureComponent2D` faces (90° plus 10° overscan). A ticker-driven job queue. Two sweeps in Tour exposure mode: sweep 1 renders `SCS_SceneColorSceneDepth` at 611 px to meter exposure and write depth; sweep 2 renders `FinalColorLDR` at one shared exposure. Also: floor plans (orthographic `SCS_BaseColor`, anything above the cut height hidden), `tour.json`, Auto Place (grid flood fill plus Poisson pick), overriding the editor's background throttle, and toast notifications. |
| `PanoStitcher` | Cube faces → 2:1 equirect (N×N supersampled bilinear), depth → radial distance, depth packed as RGB24 millimetres. |
| `PanoExposure` | Recreates the engine's histogram auto exposure (post-process volume blend, 10–90% outlier rejection) so each spot matches the viewport. |
| `PanoSceneExport` | Static-mesh geometry → positions-only `scene.glb`. Unreal (x, y, z) cm → glTF (y, z, −x) m, with winding flipped. |
| `PanoViewerServer` | Serves tours through `FHttpServerModule` on localhost:8765 under `/pano/`. |
| `PanoCapturePoint` | A placed capture spot: id, floor, neighbours, sort order. |

**Viewer** (`Resources/Viewer`, copied into each tour folder; `viewer.js` is 1.8k lines):
- **Standing still:** an exact full-screen equirect lookup.
- **Walking:** the panoramas are projected onto `scene.glb` plus a ground grid and a sky sphere. Each pixel blends up to 4 panoramas, weighted by visibility from their depth maps (5 samples each), by crossfade, by view angle, and with a soft fallback.
- **Dollhouse:** the same projective material, with surfaces grouped by their four nearest points and weighted by 1/r⁴.
- **Textures:** three tiers (2K / 4K / 8K) with eviction budgets and an idle-only GPU upload queue.
- **Cursor:** a three-mesh-bvh raycast with a depth fallback and damping.
- **Libraries:** three@0.170 and three-mesh-bvh@0.8.3, both loaded from the jsDelivr CDN.

## How to run

- **Build the editor target:** `G:\UE_5.6\Engine\Build\BatchFiles\Build.bat GDNLoadingTimeTestEditor Win64 Development -Project=...uproject`
- **If the editor is open:** Live Coding blocks that build. Build the `GDNLoadingTimeTest` game target instead to check compile errors.
- **Drive the running editor:** `python Tools/ue_remote.py "<python>"` or `--livecoding`. This uses the Python plugin's remote execution, localhost only.
- **Capture:** on the Pano 360 Camera, use *Capture Tour* or *Auto Place Points*. Output goes to `Saved/PanoCaptures/<map>/`.
- **View:** *Open Viewer*, which serves `http://localhost:8765/pano/NewMap/index.html`. Any static server also works; `file://` does not.

## Measured, not assumed

- **Depth:** reads 199.9 cm for a 200 cm lens height, and is within about 0.6% across the floor. The mesh and depth agree to about 1 cm from every point.
- **Capture:** 38 points in 89 s (2.3 s per point). Before the throttle override it was about 21 s per point.
- **Walks:** the worst frame is 2.3–11 ms and click to motion is 0–2 ms (per frame, with `gl.finish`). Upload costs are 2K 6 ms, 4K 35 ms, 8K 160 ms, which is why uploads only run while idle.
- **Cursor:** zero jitter on a 1-pixel sweep; the old depth-based version jumped up to 3.1 cm.
- **Viewer memory:** depth takes 76 MB in the browser and 152 MB on the GPU for 38 points.

## Please review

1. **Lifetime and threading in `PanoCaptureCamera.cpp`:**
   - The ticker is registered with `CreateUObject`, and `BeginDestroy` was removed on purpose; the ticker skips unbound delegates.
   - The async write lambdas take ownership of the pixel data they write.
   - `PendingWrites` gets reset on cancel while writes may still be running.
   - The job index captured in each pending write stays valid after `Jobs.Reset()`.
2. **Coordinate and seam math:** the stitcher (the lon/lat and face-basis conventions), the planar-to-radial depth conversion, and the Unreal ↔ three mapping in `viewer.js` (`toThree`, `panoLocal`, the shader's `panoUV`). A single sign error here breaks everything quietly.
3. **Exposure** (`PanoExposure.cpp`) against the engine source it mirrors, in `Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp` and `.usf`.
4. **The HTTP server** (`PanoViewerServer.cpp`): the path traversal guard, URL decoding, and binding to localhost only. Earlier, binding the route `"/"` hit a fatal assert.
5. **Viewer state machine:**
   - Races between `show()`, `transition`, fly-ins and `setMode`.
   - The texture tiers, `evictTextures` and the upload queue. Could anything that's still drawn get disposed?
   - Whether the `uploadedOnly` swaps can ever leave a stale texture on screen.
6. **Shader correctness** (the projective material in `viewer.js`): the visibility and sky rules, the `textureGrad` seam handling, and the GLSL3 sampler-array constraints.
7. **Auto Place** (`AutoPlacePoints`): the collision channel assumptions (`ECC_Visibility`), stairs and multi-floor handling (untested), and performance on large levels (capped at 60k cells).

## Known issues and decisions

- **Supersampling defaults to 1.0.** At 1.5× (3662 px faces), Lumen reflections dropped out of the scene captures in 5.6. The cause wasn't investigated.
- **Brightness drifts between editor sessions**, probably because the volumetric clouds move. That's why metering differed between runs; it wasn't the resolution.
- **Glossy reflections show a seam during walks** where visibility switches between panoramas. This is inherent to reprojecting panoramas.
- **`scene.glb` only has static meshes.** Landscape, foliage, skeletal meshes and translucency are missing (the viewer adds a ground plane instead). The export needs CPU-readable render data, so it may fail in cooked builds.
- **The viewer depends on a CDN.**
- **The JadsToolkit plugin** (not part of this work) has a Runtime module that depends on `SourceControl`, which will break packaged game builds.

## Out of scope for this review

Planned but not built: configurator variants, a measure tool, hotspots, a schematic floor plan, mobile support and offline bundling.
