# PanoCapture

Matterport-style virtual tours rendered straight from Unreal Engine 5.6: 360° panoramas, depth, a floor plan and a
light 3D model of the level, plus a three.js viewer with walk transitions, a dollhouse and a floor plan view.

## What's in here

| Path | What |
|---|---|
| `Source/PanoCapture` | The plugin (one Runtime module). `APanoCaptureCamera` captures; `APanoCapturePoint` marks spots. |
| `Resources/Viewer` | The web viewer (`index.html`, `viewer.js`, `viewer.css`). Copied into every tour folder. |
| `Tools/build_web_tour.py` | Turns a captured tour into a small, host-ready site (AVIF, lossless WebP depth, meshopt mesh). |
| `Config/DefaultEngine.ini` | Pins the editor's viewer server to localhost. |
| `REVIEW_HANDOFF.md`, `FIX_HANDOFF.md` | Architecture notes, measurements, and the review/fix history. |

## Capturing a tour

1. Copy this folder to `<YourProject>/Plugins/PanoCapture` and build the editor.
2. Place **Pano Capture Point** actors at eye height, or use **Auto Place Points** on a **Pano 360 Camera**.
3. On the camera, press **Capture Tour**. The output goes to `Saved/PanoCaptures/<map>/`.
4. **Open Viewer** serves it on `http://localhost:8765/pano/<map>/index.html`.

For a web release, set the camera's **Image Format** to **PNG**, so the web build compresses each image once.

## Building the web version

```
python Tools/build_web_tour.py <tour folder> <output folder> [--name "Title"] [--format avif|webp] [--quality 70]
```

Needs Python with Pillow (WebP and AVIF support) and Node.js (the script runs `npx gltfpack`). On the 15-point test tour
this took the site from 55 MB to 12 MB:

| | Capture | Web build |
|---|---|---|
| 8K panoramas | JPEG q92, 2.1 MB each | AVIF q70 4:4:4, 0.27 MB each (same PSNR against a lossless master) |
| Depth | PNG | lossless WebP (byte-identical) |
| Mesh | 3.1 MB glb | 0.5 MB, meshopt + 16-bit positions |

The output is a static site. Upload the whole folder to any host. `_headers` (read by Cloudflare Pages and Netlify)
caches the content-hashed `assets/` for a year and makes `tour.json` and the viewer revalidate.

The viewer loads three.js 0.170 and three-mesh-bvh from the jsDelivr CDN. AVIF needs Chrome/Edge 85+, Firefox 93+
or Safari 16.4+.
