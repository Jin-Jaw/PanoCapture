# PanoCapture: handoff

**State on 2026-09-23.** Everything below works end to end on the test level `NewMap` (15 hand-placed points, 3 demo
tags). The live demo is https://testcustommatterport.jin-jaw.co.uk. It hasn't been tried on a real interior yet.

Start with the [README](README.md) for what the kit does and how to use it. This file is for whoever works on it
next: how it's built, what has been measured, what's missing, and the traps. The tour.json format is in
[docs/TOUR_FORMAT.md](docs/TOUR_FORMAT.md). Earlier review and fix notes are in [docs/history](docs/history).

## How it fits together

```
Unreal (editor or runtime)                    Web build                         Browser
------------------------------------------   ------------------------------    ------------------------------
APanoCaptureCamera --CaptureTour-->           Tools/build_web_tour.py           Resources/Viewer (three.js)
  6 scene captures per point                    AVIF panoramas                   360 view, walks, dollhouse,
  panoramas, depth, floor plan,                 lossless WebP depth              3D floor plan, tags, compass
  scene.glb, tour.json                          meshopt scene.glb
  -> Saved/PanoCaptures/<tour>/                 hashed assets/, _headers  --->  any static host
                                                                                 (Cloudflare Worker today)
APanoTag, APanoCapturePoint --UpdateTourTags--> tour.json (tags, rooms, subtitle; no recapture)
```

### Unreal module (`Source/PanoCapture`, one Runtime module, UE 5.6)

| File | What it does |
|---|---|
| `PanoCaptureCamera` | The capture actor. It runs a job queue on the core ticker. **Tour** exposure uses two sweeps: first each point is metered and its depth written (HDR at depth resolution), then all points are captured at one shared exposure. It also does floor plans (orthographic base colour, cut at a height), `tour.json`, `scene.glb`, Auto Place, the background-throttle override and editor toasts. Settings are copied into `FCaptureSettings` / `ActivePostProcess` when a capture starts, so edits during a capture don't affect it. |
| `PanoExposure` | Recreates the viewport's auto exposure for a spot: post process volume blending, the histogram with the engine's bucket and black-bucket rules, outlier rejection, the bias curve, and `r.AutoExposure.LuminanceMethod`. It turns that into a manual bias for the faces. |
| `PanoStitcher` | Cube faces → 2:1 equirect (supersampled bilinear). Depth goes to radial distance with edge-aware bilinear sampling, then is packed as RGB24 millimetres. |
| `PanoSceneExport` | Static meshes → positions-only `scene.glb`. Unreal (x, y, z) cm → glTF (y, z, −x) m, with the winding flipped. |
| `PanoViewerServer` | The editor's *Open Viewer* server, on localhost:8765 under `/pano/<tour>/`. It serves only tours opened through it, rejects any `Host` that isn't localhost, and refuses to start on a port whose config binds beyond loopback. |
| `PanoCapturePoint` | A capture spot: id, floor, **Room Name**, neighbours, sort order. |
| `PanoTag` | A Matterport-style tag: title, description, media, link, colour, stem length. The stem points along the actor's up axis. |

### Viewer (`Resources/Viewer/viewer.js`, one ES module, three.js 0.170 + three-mesh-bvh 0.8.3 from jsDelivr)

- **360:** a full-screen triangle that looks up the equirect exactly (`panoMaterial`).
- **Walks:** both panoramas are projected onto `scene.glb`, blending up to 4 panos per pixel by depth visibility,
  crossfade and view angle (`makeProjectiveMaterial`). Without a mesh, each pano is ray-marched against its own
  depth instead.
- **Dollhouse:** the same projective material. Triangles are grouped by their 4 nearest points, weighted 1/r⁴, and
  clipped at 2.3 m.
- **Floor plan:** the dollhouse seen from straight above through a 14° lens, turned to the view heading. Tours
  without depth fall back to the captured plan image (`PlanView`).
- **Mode changes:** camera flights (`enterOverview`, `switchOverview`, `flyToPano`). The full-screen pano is
  depth-traced from the moving camera and dissolves over the model during the first or last half of the flight.
- **Navigation safety:** every navigation takes a new `navToken`. A request that finds a newer token after an
  `await` gives up, so only the latest one touches the uniforms or the camera.
- **Textures:** three tiers per point (2K preview, 4K walk copy, full). GPU uploads only happen while the view is
  idle, because an 8K upload stalls about 160 ms. Budgets: 2 full, 6 medium; previews and the dollhouse's own
  textures are never evicted. Hovering a circle decodes that point's full image ahead of the click.
- **Overlay:** tags, stems and room labels are DOM/SVG elements projected every frame (`updateOverlay`). The tag card
  sits beside its disc. The compass is SVG.
- **Circles:** drawn from a signed distance field (`makeCircleMaterial`), with a faint `marker` style and a bolder
  `cursor` style. They keep one size in the world. In each 16° direction the nearest visible point is shown, and the
  one behind it faded.

### Web build (`Tools/build_web_tour.py`)

- **Panoramas:** AVIF q70 4:4:4, resized from the master.
- **Depth:** lossless WebP.
- **Floor plan:** WebP.
- **Tag media:** local files are copied along.
- **Model:** `npx gltfpack -cc -vp 16`.
- **Assets:** renamed by content hash under `assets/`, with `_headers` caching them for a year and making
  `tour.json` and the viewer revalidate. Re-running it on the same masters produces the same hashes, so a redeploy
  uploads only what changed.

## Running it

| Task | How |
|---|---|
| Build | `G:\UE_5.6\Engine\Build\BatchFiles\Build.bat GDNLoadingTimeTestEditor Win64 Development -Project=E:\Unreal\GDNLoadingTimeTest\GDNLoadingTimeTest.uproject`. With the editor open, Live Coding blocks this: build the game target `GDNLoadingTimeTest` to check that it compiles. Header or UPROPERTY changes need an editor restart. |
| Drive the editor | `python Tools/ue_remote.py "<python>"` or `--file script.py`. This is the Python plugin's remote execution, localhost only. The host project needs `PythonScriptPlugin` and `bRemoteExecution=True`. |
| Capture | On the Pano 360 Camera: **Capture Tour**. Use **Image Format PNG** for a web release. Output goes to `Saved/PanoCaptures/<tour>/`. |
| Tags only | Place **Pano Tag** actors, set Room Names and the Tour Subtitle, then press **Update Tour Tags** (no recapture). |
| Web build | `python Tools/build_web_tour.py Saved/PanoCaptures/<master> Saved/PanoWeb/<site> --name "<title>"`. It takes about 2 minutes for 15 points and needs Pillow with AVIF (12.x) and Node. |
| Preview | Any static server, e.g. `python -m http.server 8792 --bind 127.0.0.1 --directory Saved/PanoWeb`. `file://` doesn't work. |
| Deploy | From the folder holding `wrangler.toml`: `npx wrangler@4 deploy`. See the README. The demo is the Worker `testcustommatterport` on the personal Cloudflare account, with a custom domain. |

## Measured, not assumed

**Capture**
- 15 points in 34 s at 8K.
- Mid-capture edits to Exposure Mode, Capture Depth and Image Format don't affect a running capture, and
  `tour.json` matches the files.
- Floor depth error (95th percentile) is 0.081%, down from 0.215% before the bilinear depth sampling.

**Size and quality**
- The site went from 55 MB to 11.7 MB.
- AVIF q70 matches JPEG q92 in PSNR against a lossless master (47.8 dB) at 0.26 MB instead of 2.04 MB. WebP bands in
  smooth sky at similar sizes, which is why the build uses AVIF.
- Depth survives byte-for-byte, and the mesh's positions match the original to below a millimetre.

**Speed on the live site**
- The full 8K is on the GPU about 0.2 s after the viewer starts.
- Motion starts about 0.1 s after a click, and a 6 m walk finishes in about 1 s.
- 360 → plan takes 1.9 s the first time (that includes building the dollhouse), plan ↔ dollhouse 0.7 s, back to
  360 1.0 s.

**Reference:** a Matterport screen recording, timed frame by frame, shows about 0.5 s of motion per walk and about
1 s into the dollhouse or plan. The constants at the top of `viewer.js` follow that.

**Server:** a foreign `Host` gets 403. Traversal, `:` streams, encoded slashes and tours that were never opened get
404.

## Not verified

- **Exposure:** brightness against the viewport in a strongly coloured room, or with a bias curve in a post process
  volume. The code mirrors the engine source but hasn't been compared on screen. Basic auto exposure is metered like
  Histogram (with a warning). The meter mask is ignored on purpose: it weights the screen, and a panorama has none.
- **Undo:** Auto Place undo/redo.
- **Closing the map mid-capture:** the `BeginDestroy` path. Deleting the camera mid-capture was tested.
- **The toast:** that it closes after deleting the camera. The throttle restore was tested.
- **A real multi-room interior.** Things to check there:
  - Auto Place: stairs (`MaxStep` is relative to the seed floor), furniture that only has complex collision
    (`bTraceComplex=false`), and the 60k-cell cap.
  - The floor plan cut hides whole components, so a building modelled as one mesh keeps its roof.

## Known limits

- Glossy floors show view-dependent reflections during walks. That's inherent to reprojecting panoramas.
- `scene.glb` has static meshes only (no landscape, foliage, skeletal meshes or translucency); the viewer adds a
  ground plane instead. The export needs CPU-readable render data, so it may fail in cooked builds.
- Supersampling above 1 lost Lumen reflections in 5.6 scene captures at face sizes around 3,600 px. The default is
  1.0; the cause wasn't investigated.
- Scene brightness drifts between editor sessions (probably moving volumetric clouds). Compare exposure only within
  one session.
- Room labels need Room Names on the points (the demo level has none). Matterport's hover highlight of rooms on the
  plan needs room outlines we don't have.
- Tags are hidden when their disc is behind geometry from the current point. That uses the depth at the disc, not
  at the anchor.
- Depth maps are now the biggest download (4 MB of 11.7), because the dollhouse loads all of them. They're the next
  easy saving: half resolution, or loaded per floor.

## Traps

- **The built-in browser pane of the Claude app** stops `requestAnimationFrame` (and can report a 0 × 0 window) while
  it's hidden. Scripted tests there need a manual frame pump (`setInterval(() => tourViewer.frame(), 16)`). The 0 × 0
  case is also why `resize()` ignores zero sizes: a NaN aspect used to leave the camera NaN permanently.
- **Hover must not count as interaction.** Uploads wait for the view to be idle, and a stuck `dragging` (lost pointer
  capture) used to keep the 4K or 2K copy on screen indefinitely.
- **`BeginDestroy` runs during GC,** where `FindObject` is fatal. The background-throttle restore uses the pointers
  cached when the override was applied.
- **Cloudflare combines every matching `_headers` rule,** so a catch-all `no-cache` also lands on `/assets/*`. List the
  no-cache files one by one.
- **gltfpack quantizes positions and puts the scale on the node.** The viewer bakes the node transform into float
  positions after loading.

## Next steps (not built)

- **Matterport features:** guided tour (play button), measure tool, share / deep links to a view, room highlight on
  the plan.
- **Authoring:** edit tags in the viewer, or gather tags from existing actors by component.
- **Delivery:** offline bundle (three.js is on a CDN today), a mobile layout and touch polish, configurator variants
  (material or furniture swaps per point).
- **Output:** GPano XMP metadata for third-party viewers, and an exact glTF export (textured) as an alternative to
  projection.
