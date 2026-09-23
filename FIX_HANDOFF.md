# PanoCapture: fix handoff

**For:** whoever picks up the PanoCapture plugin after the 2026-09-23 code review.
**Supersedes:** `REVIEW_HANDOFF.md` (keep it for the architecture summary, measurements and how-to-run; nothing in it is wrong).
**State:** works end to end on `NewMap` (38 points). Not yet tried on a real interior. **No version control**, so back up `Plugins/PanoCapture` before editing.

## Status (2026-09-23, after the fix pass)

All eight items are fixed; backup of the pre-fix plugin in `PluginBackups/PanoCapture_2026-09-23_before-fixes`. Line numbers below refer to the old files.

- **Verified live:** 1 (two `show()` calls: the older is dropped, the walk uses the newer target's own texture/centre; a pending walk plus `setMode('dollhouse')` never runs walk and fly together). 2 (Exposure Mode, Capture Depth and Image Format flipped twice during a 15-point tour: no crash, `tour.json` matches the files). 3 (throttle restored on finish, cancel and deleting the camera mid-capture; toast not checked by eye). 5 (foreign `Host` gets 403; traversal, `:` streams, encoded slashes and unopened tours get 404). 7 (floor error p95 0.215% → 0.081%, same points). 8: malformed hash starts the tour.
- **Built, not verified against the viewport:** 4 (exposure: luminance weights, bias curve, histogram range and black bucket, enum overrides, `r.DefaultFeature.AutoExposure*`, `r.EyeAdaptationQuality` 0/1, `r.EyeAdaptation.MethodOverride`). Basic auto exposure is still metered like Histogram, and the meter mask is ignored on purpose (it weights a screen; a panorama has none); both log a warning once. 6 (Auto Place undo). BeginDestroy (map closed mid-capture).
- **Item 1 behaviour:** the latest click wins. Clicking A then B before A loads walks to B, with B's pano.
- **Also changed in the viewer:** SDF-drawn circles (sharp at any zoom, no smearing along the floor), the view is set by a 90° horizontal FOV (the old 75° vertical was 107° across at 16:9), hovering no longer delays the full-resolution upload and decodes the hovered point's full image ahead of the click, the dollhouse is built once and has its own textures outside the eviction tiers.
- **Circles all around:** no distance cap any more. In each direction (16° sectors) the nearest visible point gets a circle and the one behind it a faded one; further ones are left out. Circles beyond 8 m grow with distance (up to 3×).
- **Dollhouse → 360 "limbo":** a 0 × 0 window (hidden or minimized) made the aspect NaN, which put the dollhouse camera and the yaw on the way back at NaN for good. Resize now ignores zero sizes, and leaving the dollhouse always snaps onto a point: the one you left from, or the nearest one on the floor being viewed.
- **Stuck low resolution:** `pointercancel` / lost capture / window blur didn't clear `dragging`, which blocks every texture upload; they do now.
- **New:** `Config/DefaultEngine.ini` pins the server ports to localhost, and `EnsureServer` refuses any port whose configured bind address is not loopback.

## What the review covered

All of `Source/PanoCapture` and `Resources/Viewer` (about 5k lines), checked against UE 5.6 engine source at `G:\UE_5.6` for the exposure model (`Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp`, `Shaders/Private/PostProcessEyeAdaptation.usf`, `PostProcessHistogramCommon.ush`), post process blending (`Engine/Private/SceneView.cpp`, `Scene.cpp`), the HTTP server (`Online/HTTPServer`) and the core ticker (`Core/Private/Containers/Ticker.cpp`).

**Confirmed correct, no action needed:**
- Ticker registered with `CreateUObject`: `FTSTicker` drops an element whose delegate is unbound, so an actor going away mid-capture does not crash.
- Async write lambdas own their pixel data; dropping the futures on cancel is safe.
- `FPendingWrite::JobIndex` stays valid: `Jobs` never shrinks during a capture and both arrays reset together.
- Stitcher lon/lat and face-basis conventions; the viewer's `toThree`, `panoLocal`, shader `panoUV`, heading handling, plan projection. Handedness flip and glb winding flip match.
- Planar to radial depth conversion.
- Exposure: the auto formula, the manual formula, the metering trick (manual bias `log2(LuminanceMax)` gives exposure 1 so pre-exposure is 1 too), and the +1 EV project default (picked up because `FPostProcessSettings()` reads `r.DefaultFeature.AutoExposure.Bias`).
- HTTP: path traversal guard (decode first, then `..` check, then root prefix check), route prefix fallback, engine default bind address is `localhost` (`HttpServerConfig.h:13`).
- GLSL3: samplers passed as function parameters, no sampler arrays; `textureGrad` seam handling; `uploadedOnly` texture swaps never show a disposed texture.

## Fix list, in order

Line numbers are for the files as they are today.

### 1. Viewer: concurrent `show()` calls corrupt the walk (high)

`Resources/Viewer/viewer.js:814-875`. `show()` writes the B-side pano uniforms (835-842) and the projective slots (854-860) before its awaits, and only checks for a competing walk at 866. Click marker A, then marker B while A's textures are still loading: the walk to A runs with B's texture, centre, depth and heading. The same gap lets `setMode('dollhouse')` (1410) start a fly-in while a walk is loading, so `transition` and `fly` both drive the camera.

Fix: give each `show()` a token (`this.showToken = ++counter`), re-check the token, `this.transition`, `this.fly` and `this.mode` after every await, and move every uniform and slot write below the final guard. Reproduce on a cold cache with the 8K image not prefetched.

### 2. Camera: property edits during a capture can crash (high)

`Source/PanoCapture/Private/PanoCaptureCamera.cpp`. `MeterTargets` is sized once in `PrepareFaces` (719) from `NeedsHdrPass()`, but `NeedsHdrPass()` (837) re-reads the live `ExposureMode` and `bCaptureDepth` every tick. Switching Manual to Auto, or turning depth on, mid-capture indexes an empty array at 742 (`SetFacesMetering`) or 939 (`ReadHdrPass`). Changing `ImageFormat`, `bWritePreviews` or `OutputWidth` mid-capture makes `WriteManifest` (1248) describe files that were never written.

Fix: copy the settings the capture depends on into a struct in `StartCapture` and read only that during the capture, or override `CanEditChange` to lock those properties while `IsCapturing()`.

### 3. Camera: deleting the actor or changing level mid-capture leaks editor state (medium)

Editor-world actors never receive `EndPlay` (270), so `StopCapture` never runs when the camera is deleted or the map is closed during a capture. The ticker goes quiet on its own, but: the background throttle override (337-361) stays applied for the rest of the session and gets persisted if the user later saves Editor Preferences; the toast keeps spinning with a dead Cancel button; `LensRoot` stays at the last spot.

Fix: add `Destroyed()` (and/or `BeginDestroy`) that only touches non-UObject state: `RemoveTicker`, `OverrideBackgroundThrottle(false)`, `FinishNotification`. That is safe in `BeginDestroy`; the original reason for removing it does not apply to these three calls.

### 4. Exposure: close to the engine, not identical (medium)

`Source/PanoCapture/Private/PanoExposure.cpp`.
- **Luminance weights** (310-313): the engine default for `r.AutoExposure.LuminanceMethod` is 0, uniform thirds (`PostProcessEyeAdaptation.cpp:83`). Rec.709 is only used when the cvar is 2. Read the cvar and pick the matching weights. Coloured rooms differ by about 0.1 EV.
- **Bias curve** (`AutoExposureBiasCurve`) and **meter mask** are ignored. Engine: `GetAutoExposureCompensationFromCurve`, evaluated at `LuminanceToEV100(avg) + log2(1/0.18)`. Only matters for levels that use them.
- **Histogram range** (416-418): fixed -16..16 EV. The engine uses the blended `HistogramLogMin/Max` (-10..20 with extended range, `Scene.cpp:468`) and clamps darker pixels into the bottom bucket. Blend those two fields in `GetSettingsAt` and use them.
- **Enum overrides** (371-378): the engine's `SET_PP` applies `AutoExposureMethod` at any weight above 0 (`SceneView.cpp:1561`), not only at 0.5 or more.
- Not handled: `r.DefaultFeature.AutoExposure=0`, `r.DefaultFeature.AutoExposure.Method`, `r.EyeAdaptationQuality=1` (forces Basic). This project sets none of them.

### 5. HTTP server: serves more than the tour, no Host check (medium)

`Source/PanoCapture/Private/PanoViewerServer.cpp`.
- `GServeRoot` (379) is the parent of the tour folder, so everything under `OutputDirectory` is served. `OutputDirectory` set to a drive root or `.` serves the drive or all of `Saved`. Fix: serve only tour folders that have been opened, keyed by name (`/pano/<name>/...` looked up in a map), or make the root the tour folder itself.
- Localhost binding is an engine ini default, not something the plugin asserts. A `[HTTPServer.Listeners] DefaultBindAddress=any` in any config exposes it silently. Add a `ListenerOverrides=(Port=8765,BindAddress="localhost")` entry to the plugin's config or check the bound address.
- `HandleRequest` (310) does not check the `Host` header. Reject anything other than `localhost:<port>` / `127.0.0.1:<port>` to close DNS rebinding.
- `GRouter` (254) is a global that outlives the HTTP module. Give `PanoCaptureModule.cpp` a real module class with `ShutdownModule` that unbinds and resets it.

### 6. Auto Place: no undo, plain Destroy (medium)

`PanoCaptureCamera.cpp:366-372` and `548-560`. `Destroy()` and `SpawnActor` bypass transactions, so re-running wipes hand-adjusted auto points irreversibly. Wrap in `FScopedTransaction`, `Modify()` the level, and use `World->EditorDestroyActor` under `WITH_EDITOR`.

Untested behaviours to check on a real interior: `bTraceComplex=false` (422) passes through furniture that only has complex collision; stairs are never climbed (`MaxStep` is relative to the seed floor, 419); the 60k cell cap (420) is about 15,000 m2 at 50 cm cells.

### 7. Depth: nearest sampling on the faces (low-medium)

`Source/PanoCapture/Private/PanoStitcher.cpp:127-139`. The 0.6% floor error in the measurements comes from picking the nearest face texel and its centre ray, not from float16. Use the same edge-aware bilinear the viewer already has in `depthAtUV` (blend when the four texels agree within 8%, nearest otherwise), then convert with the exact equirect ray. Tightens the shader's visibility tolerance.

### 8. Small items (low)

- `viewer.js:132`: `%` on a negative int is undefined in GLSL; use `if (x0 < 0) x0 += size.x`.
- `viewer.js:927-934, 944`: every `buildMarkers` allocates geometries and materials that are never disposed. Share one `PlaneGeometry` and dispose materials on `clear`.
- `viewer.js:1761`: a malformed hash throws from `decodeURIComponent` and the viewer never starts. Wrap in try/catch.
- `viewer.js:641`: eviction's keep set lacks the destination and fills until `this.transition` exists. With previews off, the dollhouse holds `image`-tier textures that eviction can dispose. Add the pending walk's points to `keep`, or exclude dollhouse textures from tiers.
- `PanoCaptureCamera.cpp:909`: a failed meter read puts the raw user bias into the tour average (995-1002). Skip those jobs when averaging.
- `PanoCaptureCamera.cpp:1174`: the floor plan cut only hides components whose whole bounds start above the cut, so a single-mesh house keeps its roof. Document, or hide by section/per-mesh clip.
- `PanoViewerServer.cpp:268`: `UrlDecode` converts one UTF-16 code unit at a time, so tour names with emoji break.
- If a new capture starts right after a cancel, the old writes still running can race the new ones on the same file names. Either wait for `PendingWrites` to drain or write to unique temp names and rename.

## How to verify

- Build: `G:\UE_5.6\Engine\Build\BatchFiles\Build.bat GDNLoadingTimeTestEditor Win64 Development -Project=E:\Unreal\GDNLoadingTimeTest\GDNLoadingTimeTest.uproject` (game target `GDNLoadingTimeTest` if the editor is open and Live Coding blocks it).
- Item 1: open the viewer, hard-refresh, click two markers in quick succession before the first loads. The walk must show the first target's pano.
- Item 2: start Capture Tour, toggle Exposure Mode and Capture Depth in the details panel while it runs. No crash, and `tour.json` matches the files on disk.
- Item 3: start a capture, delete the camera. Editor Preferences > Performance must still show the throttle setting unchanged, and the toast must close.
- Item 4: compare a pano's brightness with the viewport at the same spot in a strongly coloured room, and once with a bias curve in a post process volume.
- Item 5: `curl -H "Host: evil.example" http://localhost:8765/pano/NewMap/tour.json` must fail; `http://localhost:8765/pano/../` style paths must 404 (already do).
- Item 7: re-run the depth-straight-down log check; the floor error should drop well under 0.6%.

## Still out of scope

Configurator variants, measure tool, hotspots, schematic floor plan, mobile, offline bundling. The `JadsToolkit` plugin was removed from the project on 2026-09-23 (moved to `PluginBackups/JadsToolkit_removed_2026-09-23`); `Content/EUW_Test` and `Content/wbp_pre` used it and will show Blueprint errors.
