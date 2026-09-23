# PanoCapture

Matterport-style virtual tours rendered straight from Unreal Engine 5.6. The Unreal plugin captures 360° panoramas,
depth, floor plans and a light 3D model of the level. A build script turns that into a small static website, and the
three.js viewer plays it: walks between points, a dollhouse, a 3D floor plan and info tags.

Live demo: https://testcustommatterport.jin-jaw.co.uk. For how it's built, what's measured and what's next, see
[HANDOFF.md](HANDOFF.md). The data format is in [docs/TOUR_FORMAT.md](docs/TOUR_FORMAT.md).

## What's in here

The repo is the plugin folder, and it holds both halves of the kit: drop it into `<YourProject>/Plugins/PanoCapture`.

| Path | Part | What |
|---|---|---|
| `PanoCapture.uplugin`, `Source/PanoCapture` | Unreal | The plugin (one Runtime module): **Pano 360 Camera**, **Pano Capture Point**, **Pano Tag**, stitching, exposure, mesh export, the local viewer server. |
| `Config/DefaultEngine.ini` | Unreal | Pins the editor's viewer server ports to localhost. |
| `Resources/Viewer` | Web | The viewer (`index.html`, `viewer.js`, `viewer.css`). Copied into every captured tour. |
| `Tools/build_web_tour.py` | Web | Turns a captured tour into a host-ready site (AVIF, lossless WebP depth, meshopt mesh, hashed assets). |
| `Tools/cloudflare/wrangler.example.toml` | Web | Deploy config for Cloudflare (static assets + custom domain). |
| `Tools/ue_remote.py` | Both | Runs Python in the open editor (captures, tag updates) from a terminal. |
| `HANDOFF.md`, `docs/` | Docs | Current state, tour.json format, earlier review and fix notes. |

## In the viewer

- **360 view:** the full-resolution panorama, with a compass and scroll zoom.
- **Walking:** faint circles on the floor show where you can go: in every direction, the nearest point, plus the
  next one faded. A cursor ring follows the surface under the mouse. Walks take about half a second: both panoramas
  are projected onto the scene mesh, so walls stay put.
- **Dollhouse and floor plan:** the 3D model with ceilings cut away, and the same model seen from straight above.
  Camera flights connect all three views, and the photo dissolves into the model as the camera pulls out. Click a
  circle or the model to fly back in.
- **Tags:** coloured discs on stems. Hovering shows the title; clicking opens a card with media (YouTube, Vimeo,
  video or image), the text and a link.
- **Room labels,** multiple floors, and deep links to a point (`#PointId`).

## Capturing a tour (Unreal)

1. Copy this folder to `<YourProject>/Plugins/PanoCapture` and build the editor.
2. Place **Pano Capture Point** actors at eye height, or use **Auto Place Points** on a **Pano 360 Camera**. Give
   points a **Room Name** for labels.
3. Place **Pano Tag** actors on things worth explaining. The stem points along the actor's up axis.
4. On the camera, press **Capture Tour**. The output goes to `Saved/PanoCaptures/<tour>/`. For a web release, set
   **Image Format** to **PNG** first, so each image is compressed only once.
5. **Open Viewer** serves it at `http://localhost:8765/pano/<tour>/index.html`.
6. Changed only tags, room names or the **Tour Subtitle**? Press **Update Tour Tags**; there's no need to capture
   again.

## Publishing it (web)

```
python Tools/build_web_tour.py Saved/PanoCaptures/<tour> Saved/PanoWeb/<tour> --name "Title"
```

Needs Python with Pillow 12 (WebP and AVIF) and Node.js (the script runs `npx gltfpack`). On the 15-point demo it
takes about 2 minutes and shrinks the site from 55 MB to 11.7 MB:

| | Capture | Web build |
|---|---|---|
| 8K panoramas | JPEG q92, 2.1 MB each | AVIF q70 4:4:4, 0.27 MB each (same PSNR against a lossless master) |
| Depth | PNG | Lossless WebP (byte-identical) |
| Mesh | 3.1 MB glb | 0.5 MB, meshopt with 16-bit positions |

The output is a static site, so any host works; `file://` does not. The `_headers` file (read by Cloudflare and
Netlify) caches the content-hashed `assets/` for a year and makes `tour.json` and the viewer revalidate.

On Cloudflare, copy `Tools/cloudflare/wrangler.example.toml` next to the tour folder as `wrangler.toml`, fill it
in, and run:

```
npx wrangler@4 deploy
```

Cloudflare creates the DNS record and certificate for the custom domain. Redeploys upload only the files that
changed.

The viewer loads three.js 0.170 and three-mesh-bvh 0.8.3 from the jsDelivr CDN. AVIF needs Chrome/Edge 85+,
Firefox 93+ or Safari 16.4+.
