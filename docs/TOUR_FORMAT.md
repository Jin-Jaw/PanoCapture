# tour.json

`tour.json` sits next to the images of a tour. The plugin writes it at the end of **Capture Tour**; **Update Tour
Tags** rewrites `subtitle`, `northHeading`, `tags` and each point's `room`; the web build rewrites file paths to
`assets/…` and adds `web`. The viewer reads nothing else. Fields marked optional may be missing.

Coordinates are Unreal's: X forward, Y right, Z up, centimetres. The viewer maps Unreal (x, y, z) to three.js
(y, z, −x) / 100.

## Root

| Field | Type | |
|---|---|---|
| `version` | number | `2`. Newer fields are optional and don't change it. |
| `name` | string | Title shown top left. |
| `subtitle` | string, optional | Second line under the title (the camera's Tour Subtitle). |
| `projection` | string | `"equirectangular"`. |
| `width`, `height` | number | Full panorama size in pixels (2:1). |
| `coordinateSystem` | string | Human-readable reminder of the conventions above. |
| `northHeading` | number, optional | World yaw in degrees that the compass calls north. Default 0 (+X). |
| `exposureBias` | number, optional | Tour exposure mode only: the shared exposure every panorama was captured at. |
| `depth` | object, optional | `{ width, height, encoding: "rgb24-mm", description }`. Present when points have depth. |
| `mesh` | object, optional | `{ file, triangles, description }`: the scene mesh the walks and the dollhouse project onto. |
| `floors` | array | See below. |
| `points` | array | See below. At least one. |
| `tags` | array, optional | See below. |
| `web` | object, optional | Added by the web build: `{ format: "avif" \| "webp", quality, mesh: "meshopt" \| null }`. |

## `points[]`

| Field | Type | |
|---|---|---|
| `id` | string | Unique; used in the URL hash (`#PanoCapturePoint3`). |
| `image` | string | Full-resolution panorama. |
| `medium` | string, optional | 4K copy used while walking (only when the full image is wider than 4096). |
| `preview` | string, optional | 2K copy: first paint and the dollhouse. |
| `depth` | string, optional | Depth panorama. Distance along each ray in millimetres = R · 65536 + G · 256 + B; 0 means no surface. Must be lossless (PNG or lossless WebP). |
| `position` | `{x, y, z}` | Lens position. |
| `floorZ` | number | Floor height under the point. |
| `heading` | number | Degrees around Z; 0 means the image centre faces +X. |
| `floor` | number | Floor index, matches `floors[].index`. |
| `exposureBias` | number | Exposure the faces were rendered with. |
| `neighbors` | string[] | Hand-picked links. Empty means the viewer links nearby points on the same floor itself. |
| `room` | string, optional | Room label; the viewer places it at the middle of all points with the same room on a floor. |

## `floors[]`

| Field | Type | |
|---|---|---|
| `index` | number | Floor index. |
| `name` | string | "Floor 1" by default. |
| `z` | number | Median floor height of its points. |
| `plan` | object, optional | `{ image, minX, maxX, minY, maxY }`: top-down base-colour image and the world XY it covers. Plan images have +Y to the right and +X up. Used when there is no 3D plan (no depth). |

## `tags[]`

| Field | Type | |
|---|---|---|
| `id` | string | Unique. |
| `title` | string | Card heading and hover label. |
| `description` | string | Card text; line breaks are kept; long text is clamped behind "View more". |
| `media` | string, optional | YouTube or Vimeo link (embedded player), video file (`.mp4`, `.webm`, `.m4v`, `.mov`) or image (`.jpg`, `.png`, `.webp`, `.avif`, `.gif`). Relative paths are files in the tour folder. |
| `link`, `linkLabel` | string, optional | Opened in a new tab from the card header. |
| `color` | string | `#rrggbb` disc colour. |
| `position` | `{x, y, z}` | Anchor on the surface. |
| `stem` | `{x, y, z}` | Unit direction the stem sticks out. |
| `stemLength` | number | Distance from the anchor to the disc, centimetres. |

The viewer accepts only http(s) and relative URLs for `media` and `link`, and only `#rrggbb[aa]` colours.
