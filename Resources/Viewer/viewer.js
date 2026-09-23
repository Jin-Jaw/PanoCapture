// Pano tour viewer: loads tour.json written by the PanoCapture Unreal plugin.
//
// Unreal space: X forward, Y right, Z up, centimeters (left-handed).
// three.js space: meters, Y up, right-handed. We map UE (x, y, z) -> three (y, z, -x),
// so UE +X (the default pano center) is three -Z (the default camera forward).

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { MeshoptDecoder } from 'three/addons/libs/meshopt_decoder.module.js';
import { computeBoundsTree, acceleratedRaycast } from 'three-mesh-bvh';

// Bounding-volume hierarchy for raycasts: the cursor tests the scene mesh every frame, which would be far too
// slow against hundreds of thousands of triangles without it.
THREE.BufferGeometry.prototype.computeBoundsTree = computeBoundsTree;
THREE.Mesh.prototype.raycast = acceleratedRaycast;

const CM = 0.01;
const TRANSITION_MIN_MS = 800;       // walk duration grows with distance, like Matterport
const TRANSITION_MAX_MS = 1600;
const TRANSITION_MS_PER_METER = 110;
const FLY_MS = 900;
const DOLLHOUSE_FLY_MS = 1400;      // 360 <-> dollhouse: long enough for the photo to dissolve into the model
// The view is set by its horizontal FOV, like Matterport: a fixed vertical FOV becomes ultra-wide on a wide screen
// (75 degrees vertical is 107 across at 16:9), and a flat projection that wide stretches everything near the edges.
const VIEW_HFOV = 90;               // degrees across a landscape screen
const VIEW_MAX_VFOV = 70;           // degrees top to bottom, the limit on portrait screens
const ZOOM_MIN = 0.3;               // scroll zoom, as a factor on tan(fov / 2): 0.3 is about 3x magnification
const ZOOM_MAX = 1.15;
const AUTO_NEIGHBOR_RADIUS = 15;    // meters
const AUTO_NEIGHBOR_MAX = 8;
const MARKER_RADIUS = 0.32;         // meters, like Matterport's floor dots
const MARKER_SECTOR_DEG = 16;       // points closer together than this (seen from here) count as one direction
const MARKER_OPACITY = 0.85;
const MARKER_OPACITY_BEHIND = 0.35; // the second point in a direction, behind the nearest one
const MARKER_GROW_FROM = 8;         // meters; farther circles scale up with distance...
const MARKER_MAX_SCALE = 3;         // ...to at most this
const CURSOR_RADIUS = 0.3;
const CEILING_ABOVE_FLOOR = 2.7;    // meters, proxy geometry when a pano has no depth
const DOLLHOUSE_CUT = 2.3;          // meters above the floor; higher surfaces are cut away
const DOLLHOUSE_RANGE = 25;         // meters from a capture point
const DOLLHOUSE_GRID = 256;         // mesh columns per pano

const $ = (id) => document.getElementById(id);
const isFiniteVector = (v) => Number.isFinite(v.x) && Number.isFinite(v.y) && Number.isFinite(v.z);
const toThree = (x, y, z) => new THREE.Vector3(y * CM, z * CM, -x * CM);

// --- pano math shared by the CPU side ---------------------------------------------------------

/** three world direction -> [x, y, z] in the pano's own Unreal frame (heading removed). */
function panoLocal(dir, heading) {
  const x = -dir.z, y = dir.x, z = dir.y;
  const c = Math.cos(-heading), s = Math.sin(-heading);
  return [c * x - s * y, s * x + c * y, z];
}

/** Distance (m) from a point's lens along a three world direction; 0 means no surface. */
function depthAlong(point, dir) {
  const depth = point.depth;
  if (!depth) return 0;
  const n = dir.clone().normalize();
  const [x, y, z] = panoLocal(n, point.headingRad);
  const u = Math.atan2(y, x) / (2 * Math.PI) + 0.5;
  const v = 0.5 - Math.asin(THREE.MathUtils.clamp(z, -1, 1)) / Math.PI;
  const px = ((Math.floor(u * depth.w) % depth.w) + depth.w) % depth.w;
  const py = Math.min(Math.floor(v * depth.h), depth.h - 1);
  return depth.data[py * depth.w + px];
}

/** Like depthAlong, but bilinear where the four nearest texels agree (smooth surfaces), nearest across edges. */
function depthAlongSmooth(point, dir) {
  const depth = point.depth;
  if (!depth) return 0;
  const [x, y, z] = panoLocal(dir.clone().normalize(), point.headingRad);
  const fx = (Math.atan2(y, x) / (2 * Math.PI) + 0.5) * depth.w - 0.5;
  const fy = (0.5 - Math.asin(THREE.MathUtils.clamp(z, -1, 1)) / Math.PI) * depth.h - 0.5;
  const x0 = Math.floor(fx), y0 = Math.floor(fy);
  const tx = fx - x0, ty = fy - y0;
  const wrap = (v) => ((v % depth.w) + depth.w) % depth.w;
  const row = (v) => THREE.MathUtils.clamp(v, 0, depth.h - 1);
  const a = depth.data[row(y0) * depth.w + wrap(x0)];
  const b = depth.data[row(y0) * depth.w + wrap(x0 + 1)];
  const c = depth.data[row(y0 + 1) * depth.w + wrap(x0)];
  const d = depth.data[row(y0 + 1) * depth.w + wrap(x0 + 1)];
  const lo = Math.min(a, b, c, d), hi = Math.max(a, b, c, d);
  if (lo <= 0 || hi > lo * 1.08) return ty < 0.5 ? (tx < 0.5 ? a : b) : (tx < 0.5 ? c : d);
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

/** Can this point's camera see a world position? */
function sees(point, position, slack = 0.15) {
  const dir = position.clone().sub(point.pos);
  const distance = dir.length();
  const surface = depthAlong(point, dir);
  return surface === 0 || distance <= surface + Math.max(slack, surface * 0.03);
}

// --- Matterport-style circles ----------------------------------------------------------------
// Drawn from a signed distance per pixel instead of a texture, so they stay sharp however close you get,
// and seen flat along the floor they don't smear the way a mipmapped texture does.

/** Size of the circle quad relative to the circle's radius; the halo fills the margin. */
const CIRCLE_QUAD = 2.5;

function makeCircleMaterial(opacity) {
  return new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3,
    transparent: true,
    depthTest: false,
    depthWrite: false,
    uniforms: { uOpacity: { value: opacity } },
    vertexShader: /* glsl */`
      out vec2 vUv;
      void main() {
        vUv = uv;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }`,
    fragmentShader: /* glsl */`
      precision highp float;
      in vec2 vUv;
      uniform float uOpacity;
      out vec4 outColor;

      // Straight-alpha "over": src on top of dst.
      vec4 over(vec4 src, vec4 dst) {
        float a = src.a + dst.a * (1.0 - src.a);
        return a > 0.0 ? vec4((src.rgb * src.a + dst.rgb * dst.a * (1.0 - src.a)) / a, a) : vec4(0.0);
      }

      void main() {
        // Distance from the center in circle radii (1 = the ring).
        float d = length(vUv - 0.5) * ${CIRCLE_QUAD.toFixed(2)};
        // One pixel in the same units, from the screen-space derivatives: edges are antialiased to exactly one
        // pixel at any distance or angle, and the ring never gets thinner than about a pixel and a half.
        float px = max(fwidth(d), 1e-5);
        float ringHalf = max(0.07, px * 0.75);

        float halo = 0.35 * (1.0 - smoothstep(0.88, 1.25, d));          // soft dark shadow, reads on bright floors
        float fill = 0.28 * (1.0 - smoothstep(0.93 - px, 0.93 + px, d)); // translucent disc inside the ring
        float ring = 0.95 * (1.0 - smoothstep(-px, px, abs(d - 1.0) - ringHalf));

        vec4 color = over(vec4(1.0, 1.0, 1.0, ring), over(vec4(1.0, 1.0, 1.0, fill), vec4(0.0, 0.0, 0.0, halo)));
        outColor = vec4(color.rgb, color.a * uOpacity);
      }`,
  });
}

// --- pano shader -----------------------------------------------------------------------------
// A full-screen triangle that looks up two panoramas. Standing still it is an exact lookup. While
// moving, each pano is ray-traced against its own depth map, so the walk has real parallax.
// Panos without depth fall back to a room-shaped proxy (cylinder + floor + ceiling).

// Pano lookups shared by the full-screen shader and the projective mesh material.
const PANO_GLSL = /* glsl */`
    vec2 panoUV(vec3 dir, float heading) {
      vec3 ue = vec3(-dir.z, dir.x, dir.y);            // three -> Unreal
      float ch = cos(-heading), sh = sin(-heading);    // into the pano's own frame
      vec3 l = vec3(ch * ue.x - sh * ue.y, sh * ue.x + ch * ue.y, ue.z);
      return vec2(atan(l.y, l.x) / 6.28318531 + 0.5, 0.5 - asin(clamp(l.z, -1.0, 1.0)) / 3.14159265);
    }

    // Bilinear on smooth surfaces, nearest across edges (blending foreground into background would invent geometry).
    float depthAtUV(sampler2D depth, vec2 uv) {
      ivec2 size = textureSize(depth, 0);
      vec2 p = uv * vec2(size) - 0.5;
      vec2 f = p - floor(p);
      ivec2 i = ivec2(floor(p));
      // Wrap around the longitude seam. Not with %: GLSL leaves it undefined for negative numbers. The taps stay
      // within a couple of texels of [0, size), so one step either way is enough.
      int x0 = i.x;
      if (x0 < 0) x0 += size.x;
      else if (x0 >= size.x) x0 -= size.x;
      int x1 = x0 + 1;
      if (x1 >= size.x) x1 -= size.x;
      int y0 = clamp(i.y, 0, size.y - 1);
      int y1 = clamp(i.y + 1, 0, size.y - 1);
      float a = texelFetch(depth, ivec2(x0, y0), 0).r;
      float b = texelFetch(depth, ivec2(x1, y0), 0).r;
      float c = texelFetch(depth, ivec2(x0, y1), 0).r;
      float d = texelFetch(depth, ivec2(x1, y1), 0).r;
      float lo = min(min(a, b), min(c, d));
      float hi = max(max(a, b), max(c, d));
      if (lo <= 0.0 || hi > lo * 1.08) {
        return f.y < 0.5 ? (f.x < 0.5 ? a : b) : (f.x < 0.5 ? c : d);
      }
      return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }

    float depthAt(sampler2D depth, vec3 dir, float heading) {
      return depthAtUV(depth, panoUV(dir, heading));
    }

    vec4 samplePano(sampler2D tex, vec3 dir, float heading) {
      vec2 uv = panoUV(dir, heading);
      // Pick derivatives that don't jump at the longitude seam, so mip selection stays sane there.
      vec2 uvAlt = vec2(fract(uv.x + 0.5), uv.y);
      vec2 dx = dFdx(uv), dy = dFdy(uv);
      vec2 dxAlt = dFdx(uvAlt), dyAlt = dFdy(uvAlt);
      if (abs(dxAlt.x) < abs(dx.x)) dx.x = dxAlt.x;
      if (abs(dyAlt.x) < abs(dy.x)) dy.x = dyAlt.x;
      return textureGrad(tex, uv, dx, dy);
    }

`;

const emptyDepth = new THREE.DataTexture(new Float32Array([0]), 1, 1, THREE.RedFormat, THREE.FloatType);
emptyDepth.needsUpdate = true;

const panoMaterial = new THREE.ShaderMaterial({
  glslVersion: THREE.GLSL3,
  depthTest: false,
  depthWrite: false,
  // Drawn after the solid meshes, so it can dissolve over the dollhouse while the camera flies between them.
  transparent: true,
  uniforms: {
    uOpacity: { value: 1 },
    uTexA: { value: null }, uTexB: { value: null },
    uDepthA: { value: emptyDepth }, uDepthB: { value: emptyDepth },
    uHasDepthA: { value: 0 }, uHasDepthB: { value: 0 },
    uInfinite: { value: 0 },
    uCenterA: { value: new THREE.Vector3() }, uCenterB: { value: new THREE.Vector3() },
    uHeadingA: { value: 0 }, uHeadingB: { value: 0 },
    uFloorA: { value: 0 }, uFloorB: { value: 0 },
    uRadius: { value: 4 },
    uMix: { value: 0 },
    uCamPos: { value: new THREE.Vector3() },
    uInvProj: { value: new THREE.Matrix4() },
    uCamWorld: { value: new THREE.Matrix4() },
  },
  vertexShader: /* glsl */`
    out vec2 vNdc;
    void main() {
      vNdc = position.xy;
      gl_Position = vec4(position.xy, 0.0, 1.0);
    }`,
  fragmentShader: /* glsl */`
    precision highp float;
    in vec2 vNdc;
    uniform sampler2D uTexA, uTexB, uDepthA, uDepthB;
    uniform float uHasDepthA, uHasDepthB, uInfinite;
    uniform vec3 uCenterA, uCenterB;
    uniform float uHeadingA, uHeadingB, uFloorA, uFloorB, uRadius, uMix, uOpacity;
    uniform vec3 uCamPos;
    uniform mat4 uInvProj, uCamWorld;
    out vec4 outColor;

    const float CEILING = ${CEILING_ABOVE_FLOOR.toFixed(2)};

    ${PANO_GLSL}
    // March the ray against the pano's depth. A real hit is the ray crossing a surface from in front of it.
    // Crossing into space the pano never saw (behind a foreground edge) is skipped, and the result is
    // marked uncertain (w < 1) so the other pano, which may have seen that space, takes over.
    vec4 traceDepth(vec3 o, vec3 d, vec3 c, float heading, sampler2D depth) {
      const int STEPS = 48;
      const float FAR = 40.0;
      float prevT = 0.0;
      bool prevInFront = true;
      float confidence = 1.0;
      for (int i = 1; i <= STEPS; i++) {
        float f = float(i) / float(STEPS);
        float t = FAR * f * f;
        vec3 v = o + d * t - c;
        float r = length(v);
        float s = depthAt(depth, v / r, heading);
        bool inFront = s <= 0.0 || r < s;
        if (!inFront && prevInFront) {
          float lo = prevT, hi = t;
          for (int k = 0; k < 7; k++) {
            float m = 0.5 * (lo + hi);
            vec3 vm = o + d * m - c;
            float rm = length(vm);
            float sm = depthAt(depth, vm / rm, heading);
            if (sm > 0.0 && rm >= sm) hi = m; else lo = m;
          }
          vec3 vh = o + d * hi - c;
          float rh = length(vh);
          float sh = depthAt(depth, vh / rh, heading);
          if ((rh - sh) < max(0.08, sh * 0.06)) {
            return vec4(vh / rh, confidence);
          }
          confidence = 0.0; // slipped behind an edge: this pano can't know what is here
        }
        prevT = t;
        prevInFront = inFront;
      }
      return vec4(d, confidence); // nothing hit: sky, infinitely far away
    }

    vec3 traceProxy(vec3 o, vec3 d, vec3 c, float floorY) {
      vec2 oc = o.xz - c.xz;
      vec2 dd = d.xz;
      float a = dot(dd, dd);
      float b = dot(oc, dd);
      float k = dot(oc, oc) - uRadius * uRadius;
      float t = 1e6;
      if (a > 1e-6) {
        float disc = b * b - a * k;
        if (disc > 0.0) {
          float tc = (-b + sqrt(disc)) / a;
          if (tc > 0.0) t = tc;
        }
      }
      if (d.y < -1e-5) t = min(t, (floorY - o.y) / d.y);
      if (d.y > 1e-5) t = min(t, (floorY + CEILING - o.y) / d.y);
      vec3 v = o + d * max(t, 0.0) - c;
      return dot(v, v) < 1e-8 ? d : normalize(v);
    }

    vec4 lookupDirection(vec3 o, vec3 d, vec3 c, float heading, float floorY, sampler2D depth, float hasDepth) {
      // Behind the proxy mesh only far things (sky, landscape) remain: treat them as infinitely far.
      if (uInfinite > 0.5 || distance(o, c) < 1e-3) return vec4(d, 1.0);
      return hasDepth > 0.5 ? traceDepth(o, d, c, heading, depth) : vec4(traceProxy(o, d, c, floorY), 1.0);
    }

    void main() {
      vec4 v = uInvProj * vec4(vNdc, 1.0, 1.0);
      vec3 d = normalize(mat3(uCamWorld) * (v.xyz / v.w));
      vec4 hitA = lookupDirection(uCamPos, d, uCenterA, uHeadingA, uFloorA, uDepthA, uHasDepthA);
      vec4 a = samplePano(uTexA, hitA.xyz, uHeadingA);
      if (uMix <= 0.0) { outColor = vec4(a.rgb, uOpacity); return; }
      vec4 hitB = lookupDirection(uCamPos, d, uCenterB, uHeadingB, uFloorB, uDepthB, uHasDepthB);
      vec4 b = samplePano(uTexB, hitB.xyz, uHeadingB);
      // Each pano fills the other's gaps: weight by the crossfade and by how sure each one is.
      float wa = (1.0 - uMix) * hitA.w;
      float wb = uMix * hitB.w;
      float sum = wa + wb;
      vec4 color = sum > 1e-3 ? (a * wa + b * wb) / sum : mix(a, b, uMix);
      outColor = vec4(color.rgb, uOpacity);
    }`,
});

// ---------------------------------------------------------------------------------------------
// Projective texturing onto the exported scene mesh. Each fragment takes color from up to four panos,
// weighted by how much each one should count (crossfade, distance), whether it actually saw that spot
// (checked against its depth map) and, while walking, how close its view angle is to the camera's.
// Both panos land on exactly the same surfaces, so walls stay put and nothing tears.

const SLOTS = 4;

function makeProjectiveMaterial() {
  const uniforms = {
    uCamPos: { value: new THREE.Vector3() },
    uUseAngle: { value: 1 },
    uDistanceWeight: { value: 0 },
    uFallback: { value: new THREE.Vector4(0.35, 0.35, 0.37, 1) },
    uUseFallbackColor: { value: 0 },
    uClipY: { value: 1e9 },
    uFar: { value: 60 },
    uCenter: { value: Array.from({ length: SLOTS }, () => new THREE.Vector3()) },
    uHeading: { value: new Array(SLOTS).fill(0) },
    uWeight: { value: new Array(SLOTS).fill(0) },
    uHasDepth: { value: new Array(SLOTS).fill(0) },
  };
  for (let i = 0; i < SLOTS; i++) {
    uniforms[`uTex${i}`] = { value: null };
    uniforms[`uDepth${i}`] = { value: emptyDepth };
  }
  return new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3,
    side: THREE.DoubleSide,
    uniforms,
    vertexShader: /* glsl */`
      out vec3 vWorld;
      void main() {
        vec4 world = modelMatrix * vec4(position, 1.0);
        vWorld = world.xyz;
        gl_Position = projectionMatrix * viewMatrix * world;
      }`,
    fragmentShader: /* glsl */`
      precision highp float;
      in vec3 vWorld;
      uniform vec3 uCamPos;
      uniform float uUseAngle, uDistanceWeight, uUseFallbackColor, uClipY, uFar;
      uniform vec4 uFallback;
      uniform vec3 uCenter[${SLOTS}];
      uniform float uHeading[${SLOTS}], uWeight[${SLOTS}], uHasDepth[${SLOTS}];
      uniform sampler2D uTex0, uTex1, uTex2, uTex3, uDepth0, uDepth1, uDepth2, uDepth3;
      out vec4 outColor;

      ${PANO_GLSL}

      // How much of this spot the pano actually saw (0..1), from its depth map. Four taps around the lookup soften
      // silhouettes so blends don't stair-step. Fragments on the sky sphere (far away) only take a pano's color
      // where that pano saw nothing nearby in that direction; otherwise near objects get painted into the sky.
      // gap: how far in front of this spot the pano's view was blocked, relative to distance (0 = seen).
      float tapSeen(sampler2D depth, vec2 uv, float r, inout float gap) {
        float s = depthAtUV(depth, uv);
        if (r > uFar * 2.0) {
          bool open = s <= 0.0 || s > uFar;
          gap = min(gap, open ? 0.0 : 1.0);
          return open ? 1.0 : 0.0;
        }
        if (s <= 0.0) return 0.0;
        float tolerance = 0.04 + 0.015 * r;
        gap = min(gap, max(r - s, 0.0) / max(r, 0.5));
        return 1.0 - smoothstep(tolerance, tolerance * 3.0, r - s);
      }

      float visibility(sampler2D depth, vec3 dir, float heading, float r, out float gap) {
        vec2 uv = panoUV(dir, heading);
        vec2 texel = 1.0 / vec2(textureSize(depth, 0));
        gap = 1e3;
        // The color comes from the center direction, so the center must be seen; the ring of taps only softens
        // the edge on the visible side.
        float center = tapSeen(depth, uv, r, gap);
        float ring = 0.0, ringMin = 1.0;
        for (int k = 0; k < 4; k++) {
          vec2 o = vec2(k == 0 || k == 2 ? -0.7 : 0.7, k < 2 ? -0.7 : 0.7);
          float t = tapSeen(depth, uv + o * texel, r, gap);
          ring += t;
          ringMin = min(ringMin, t);
        }
        // Sky: every tap must be open, or a thin sliver of a nearby object's edge gets painted into the sky.
        if (r > uFar * 2.0) return center * ringMin;
        return center * mix(0.35, 1.0, ring * 0.25);
      }

      void addSlot(int i, sampler2D tex, sampler2D depth, vec3 camDir, inout vec4 acc, inout float wsum, inout vec4 soft, inout float softSum) {
        if (uWeight[i] <= 0.0) return;
        vec3 v = vWorld - uCenter[i];
        float r = max(length(v), 1e-4);
        vec3 dir = v / r;
        float gap = 0.0;
        float seen = uHasDepth[i] > 0.5 ? visibility(depth, dir, uHeading[i], r, gap) : 1.0;
        float w = uWeight[i];
        // Unstructured-lumigraph style: prefer the pano that sees this spot from the camera's direction.
        if (uUseAngle > 0.5) w *= exp(-8.0 * (1.0 - max(dot(dir, camDir), 0.0)));
        // Dollhouse: the closest pano that sees a spot has the sharpest, least stretched view of it.
        if (uDistanceWeight > 0.5) w /= (0.05 + r * r * r * r);
        vec4 c = samplePano(tex, dir, uHeading[i]);
        acc += c * (w * seen);
        wsum += w * seen;
        // For spots no pano saw, lean on the one whose view was blocked closest to it.
        float nearly = exp(-gap * 20.0);
        soft += c * (w * nearly);
        softSum += w * nearly;
      }

      void main() {
        // Dollhouse cutaway: clip per pixel so big triangles (a whole wall) are cut cleanly at the same height.
        if (vWorld.y > uClipY) discard;
        vec3 camDir = normalize(vWorld - uCamPos);
        vec4 acc = vec4(0.0), soft = vec4(0.0);
        float wsum = 0.0, softSum = 0.0;
        addSlot(0, uTex0, uDepth0, camDir, acc, wsum, soft, softSum);
        addSlot(1, uTex1, uDepth1, camDir, acc, wsum, soft, softSum);
        addSlot(2, uTex2, uDepth2, camDir, acc, wsum, soft, softSum);
        addSlot(3, uTex3, uDepth3, camDir, acc, wsum, soft, softSum);
        if (uUseFallbackColor > 0.5) {
          outColor = wsum > 1e-5 ? acc / wsum : uFallback;
        } else {
          // A tiny share of the soft term keeps the transition continuous where visibility runs out.
          float total = wsum + softSum * 1e-3;
          outColor = total > 1e-9 ? (acc + soft * 1e-3) / total : uFallback;
        }
      }`,
  });
}

function setSlot(material, index, point, texture, weight) {
  const u = material.uniforms;
  u[`uTex${index}`].value = texture;
  u[`uDepth${index}`].value = point?.depth?.texture || emptyDepth;
  u.uHasDepth.value[index] = point?.depth ? 1 : 0;
  u.uHeading.value[index] = point?.headingRad || 0;
  u.uWeight.value[index] = texture ? weight : 0;
  if (point) u.uCenter.value[index].copy(point.pos);
}

// ---------------------------------------------------------------------------------------------

class TourViewer {
  constructor(tour, baseUrl) {
    this.tour = tour;
    this.baseUrl = baseUrl;
    this.points = new Map();
    this.floors = new Map((tour.floors || []).map((f) => [f.index, f]));
    this.textures = new Map();
    this.uploadQueue = [];
    this.lastInteraction = 0;
    this.current = null;
    this.transition = null;
    this.fly = null;
    // Every navigation (walk, jump, dollhouse, fly-in) takes a new token; one that finds a newer token after an
    // await was overtaken and gives up, so only the latest request ever touches the uniforms or the camera.
    this.navToken = 0;
    this.pendingWalk = [];
    this.loadingCount = 0;
    this.yaw = 0;
    this.pitch = 0;
    this.zoom = 1;
    this.mode = 'pano';
    this.dollhouseFloor = 0;

    for (const p of tour.points) {
      this.points.set(p.id, {
        ...p,
        pos: toThree(p.position.x, p.position.y, p.position.z),
        floorPos: toThree(p.position.x, p.position.y, p.floorZ ?? p.position.z - 150),
        headingRad: THREE.MathUtils.degToRad(p.heading || 0),
        floor: p.floor ?? 0,
        depthFile: p.depth || null,
        depth: null,
      });
    }
    this.hasDepth = [...this.points.values()].some((p) => p.depthFile);
    this.proxyGeometry = null;
    this.buildNeighbors();

    this.renderer = new THREE.WebGLRenderer({ antialias: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.outputColorSpace = THREE.LinearSRGBColorSpace; // pixels pass through untouched
    this.renderer.setClearColor(0x0e0f11);
    $('stage').appendChild(this.renderer.domElement);
    this.maxTextureSize = this.renderer.capabilities.maxTextureSize;

    this.camera = new THREE.PerspectiveCamera(75, 1, 0.05, 2000);
    this.camera.rotation.order = 'YXZ';

    this.scene = new THREE.Scene();
    const triangle = new THREE.BufferGeometry();
    triangle.setAttribute('position', new THREE.Float32BufferAttribute([-1, -1, 0, 3, -1, 0, -1, 3, 0], 3));
    this.panoMesh = new THREE.Mesh(triangle, panoMaterial);
    this.panoMesh.frustumCulled = false;
    this.panoMesh.renderOrder = -1;
    this.scene.add(this.panoMesh);

    this.markers = new THREE.Group();
    this.scene.add(this.markers);
    this.cursor = this.makeCircle(CURSOR_RADIUS, 0.9);
    this.cursor.visible = false;
    this.scene.add(this.cursor);

    this.transitionMaterial = makeProjectiveMaterial();
    this.transitionMesh = null;
    this.meshReady = tour.mesh?.file ? this.loadSceneMesh(tour.mesh.file) : Promise.resolve(null);

    this.dollhouse = null;
    this.dollhouseMarkers = new THREE.Group();
    this.dollhouseMarkers.visible = false;
    this.scene.add(this.dollhouseMarkers);
    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enabled = false;
    this.controls.enableDamping = true;
    this.controls.maxPolarAngle = Math.PI * 0.49;

    this.raycaster = new THREE.Raycaster();
    this.pointer = new THREE.Vector2();

    this.minimap = new PlanView($('minimap-canvas'), this, { compact: true });
    this.planView = new PlanView($('plan-canvas'), this, { compact: false });

    this.bindInput();
    this.buildFloorButtons();
    $('btn-dollhouse').hidden = !this.hasDepth;
    window.addEventListener('resize', () => this.resize());
    this.resize();

    $('tour-name').textContent = tour.name || 'Tour';
    this.renderer.setAnimationLoop(() => this.frame());
  }

  // --- data ----------------------------------------------------------------------------------

  buildNeighbors() {
    const all = [...this.points.values()];
    for (const p of all) {
      p.links = new Set((p.neighbors || []).filter((id) => this.points.has(id)));
    }
    // Points without hand-placed neighbors get linked to nearby points on the same floor.
    for (const p of all) {
      if ((p.neighbors || []).length > 0) continue;
      all.filter((q) => q !== p && q.floor === p.floor)
        .map((q) => ({ q, d: q.pos.distanceTo(p.pos) }))
        .filter(({ d }) => d <= AUTO_NEIGHBOR_RADIUS)
        .sort((a, b) => a.d - b.d)
        .slice(0, AUTO_NEIGHBOR_MAX)
        .forEach(({ q }) => { p.links.add(q.id); q.links.add(p.id); });
    }
  }

  url(file) { return new URL(file, this.baseUrl).href; }

  /** Decoded off the main thread, so big panoramas don't freeze the view while they load. */
  loadPanoTexture(file, options = {}) {
    const loader = new THREE.ImageBitmapLoader();
    loader.setOptions({ imageOrientation: 'from-image', premultiplyAlpha: 'none', colorSpaceConversion: 'none', ...options });
    return loader.loadAsync(this.url(file)).then((bitmap) => {
      const texture = new THREE.Texture(bitmap);
      texture.colorSpace = THREE.NoColorSpace;
      texture.wrapS = THREE.RepeatWrapping;
      texture.wrapT = THREE.ClampToEdgeWrapping;
      texture.flipY = false;
      texture.minFilter = THREE.LinearMipmapLinearFilter;
      texture.anisotropy = this.renderer.capabilities.getMaxAnisotropy();
      texture.needsUpdate = true;
      return texture;
    });
  }

  loadTexture(file) {
    if (!this.textures.has(file)) {
      const promise = this.loadPanoTexture(file);
      promise.catch(() => this.textures.delete(file));
      this.textures.set(file, promise);
    }
    return this.textures.get(file);
  }

  /**
   * What the dollhouse draws for a point: its preview, which eviction never touches. A tour without previews gets
   * a 2K copy of the full image of its own, outside the tiers, so eviction can't dispose what the dollhouse shows
   * (and the dollhouse doesn't hold every full-resolution image on the GPU).
   */
  dollhouseTexture(point) {
    if (point.preview) return this.requestTier(point, 'preview');
    point.dollhouseTexture ??= this.loadPanoTexture(point.image, { resizeWidth: 2048, resizeHeight: 1024, resizeQuality: 'high' })
      .catch((error) => { console.warn('Could not load', point.image, error); point.dollhouseTexture = null; return null; });
    return point.dollhouseTexture;
  }

  /**
   * Each point has up to three copies: preview (2K: dollhouse, first paint), medium (4K: walking) and image
   * (full resolution: standing still). The frame loop always shows the best one that has arrived, so a late
   * download can never be lost, and eviction keeps GPU memory bounded (an 8K pano is ~170 MB with mipmaps).
   */
  tierFile(point, tier) {
    if (tier === 'image') return point.image;
    if (tier === 'medium') return point.medium || null;
    return point.preview || null;
  }

  /** upload: false only decodes (for a likely next point); the GPU upload is queued once the point is shown. */
  requestTier(point, tier, priority = 1, { upload = true } = {}) {
    const file = this.tierFile(point, tier);
    if (!file) return Promise.resolve(null);
    if (tier === 'image' && point.preview && (this.tour.width || 0) > this.maxTextureSize) return Promise.resolve(null);
    point.tex ??= {};
    if (point.tex[tier]) {
      point.tex[tier].userData.lastUsed = performance.now();
      if (upload) this.queueUpload(point.tex[tier], priority);
      return Promise.resolve(point.tex[tier]);
    }
    return this.loadTexture(file).then((texture) => {
      point.tex[tier] = texture;
      texture.userData.lastUsed = performance.now();
      texture.userData.large = tier === 'image';
      if (upload) this.queueUpload(texture, priority);
      this.evictTextures();
      return texture;
    }).catch((error) => { console.warn('Could not load', file, error); return null; });
  }

  /**
   * GPU uploads are the only thing that can stall a frame here (measured: 2K 6 ms, 4K 35 ms, 8K 160 ms), so
   * they never happen on demand. Loaded textures wait in this queue and go up one per frame, only while the
   * view is idle: never during a walk, a fly-in or a drag. The 8K copy waits until the view has been still
   * for a moment, so its longer pause lands on a frame where nothing moves.
   */
  queueUpload(texture, priority = 1) {
    if (!texture || texture.userData.uploaded) return;
    texture.userData.uploadPriority = Math.max(texture.userData.uploadPriority ?? 0, priority);
    if (!this.uploadQueue.includes(texture)) this.uploadQueue.push(texture);
  }

  uploadNow(texture) {
    if (!texture || texture.userData.uploaded) return;
    this.renderer.initTexture(texture);
    texture.userData.uploaded = true;
    const index = this.uploadQueue.indexOf(texture);
    if (index >= 0) this.uploadQueue.splice(index, 1);
  }

  pumpUploads(now) {
    if (!this.uploadQueue.length || this.transition || this.fly || this.dragging) return;
    const idleFor = now - this.lastInteraction;
    if (idleFor < 120) return;
    this.uploadQueue.sort((a, b) => b.userData.uploadPriority - a.userData.uploadPriority);
    const index = this.uploadQueue.findIndex((t) => !t.userData.large || idleFor > 350);
    if (index < 0) return;
    const [texture] = this.uploadQueue.splice(index, 1);
    if (!texture.userData.disposed) this.uploadNow(texture);
  }

  /** A walk to this point is likely (it's hovered, or next to us): get its textures and depth onto the GPU. */
  prefetchForWalk(point, priority, { full = false } = {}) {
    this.loadDepth(point).then((depth) => depth && this.queueUpload(depth.texture, priority));
    this.requestTier(point, point.preview ? 'preview' : 'image', priority).then(() => this.requestTier(point, 'medium', priority)).then(() => {
      // Hovered: also decode the full image, so it is sharp moments after arriving rather than after a download.
      if (full) this.requestTier(point, 'image', priority, { upload: false });
    });
  }

  /**
   * Sharpest copy that has arrived. allowFull: false during walks. uploadedOnly: only copies already on the GPU,
   * so swapping to it can't stall the frame.
   */
  bestTexture(point, { allowFull = true, uploadedOnly = false } = {}) {
    const t = point?.tex;
    if (!t) return null;
    const usable = (x) => x && (!uploadedOnly || x.userData.uploaded);
    return [allowFull ? t.image : null, t.medium, t.preview, t.image].find(usable) || null;
  }

  evictTextures() {
    // Everything on screen, plus the points a walk that is still loading will draw (pendingWalk).
    const keep = new Set([this.current, this.transition?.from, this.transition?.to, ...(this.transition?.fills || []),
      ...this.pendingWalk].filter(Boolean));
    const budgets = { image: 2, medium: 6 };
    for (const tier of Object.keys(budgets)) {
      const holders = [...this.points.values()].filter((p) => p.tex?.[tier]);
      const evictable = holders.filter((p) => !keep.has(p))
        .sort((a, b) => a.tex[tier].userData.lastUsed - b.tex[tier].userData.lastUsed);
      let excess = holders.length - budgets[tier];
      for (const p of evictable) {
        if (excess-- <= 0) break;
        const texture = p.tex[tier];
        p.tex[tier] = null;
        this.textures.delete(this.tierFile(p, tier));
        texture.dispose(); // frees the GPU copy; three re-uploads if something still draws it
        texture.userData.uploaded = false;
        texture.userData.disposed = true;
        const queued = this.uploadQueue.indexOf(texture);
        if (queued >= 0) this.uploadQueue.splice(queued, 1);
      }
    }
  }

  /** The scene's real geometry, exported from Unreal (positions only). */
  async loadSceneMesh(file) {
    try {
      // Web builds compress the mesh with meshopt (see Tools/build_web_tour.py); plain exports load the same way.
      const gltf = await new GLTFLoader().setMeshoptDecoder(MeshoptDecoder).loadAsync(this.url(file));
      gltf.scene.updateMatrixWorld(true);
      let node = null;
      gltf.scene.traverse((child) => { if (child.isMesh && !node) node = child; });
      if (!node) return null;
      // Compressed builds store positions as quantized integers with the scale on the node. Everything here
      // (raycasts, the dollhouse grouping, the projective shader) wants plain world-space floats.
      const source = node.geometry.getAttribute('position');
      const positions = new Float32Array(source.count * 3);
      for (let i = 0; i < source.count; i++) {
        positions[i * 3] = source.getX(i);
        positions[i * 3 + 1] = source.getY(i);
        positions[i * 3 + 2] = source.getZ(i);
      }
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
      geometry.setIndex(new THREE.BufferAttribute(node.geometry.index.array.slice(), 1));
      geometry.applyMatrix4(node.matrixWorld);
      geometry.computeBoundingSphere();
      geometry.computeBoundsTree();
      this.proxyGeometry = geometry;
      this.transitionMesh = new THREE.Group();
      this.transitionMesh.visible = false;
      const sceneMesh = new THREE.Mesh(geometry, this.transitionMaterial);
      sceneMesh.frustumCulled = false;
      this.transitionMesh.add(sceneMesh);
      this.cursorTargets = [sceneMesh];
      // Floors that aren't static meshes (landscape, the default level's ground) still need a surface to land on.
      for (const ground of this.groundGeometries(200)) {
        ground.geometry.computeBoundsTree();
        const mesh = new THREE.Mesh(ground.geometry, this.transitionMaterial);
        mesh.frustumCulled = false;
        this.transitionMesh.add(mesh);
        this.cursorTargets.push(mesh);
      }
      // Whatever the mesh doesn't cover (sky, distant landscape) lands on a big sphere that follows the camera.
      this.skySphere = new THREE.Mesh(new THREE.SphereGeometry(900, 96, 48), this.transitionMaterial);
      this.skySphere.frustumCulled = false;
      this.skySphere.renderOrder = -1;
      this.transitionMesh.add(this.skySphere);
      this.scene.add(this.transitionMesh);
      // Compile the walk's shaders now rather than on the first click.
      this.transitionMesh.visible = true;
      this.renderer.compile(this.scene, this.camera);
      this.transitionMesh.visible = false;
      console.info(`Scene mesh: ${geometry.index.count / 3} triangles`);
      return geometry;
    } catch (error) {
      console.warn('Scene mesh failed to load; using depth-traced transitions.', error);
      return null;
    }
  }

  /**
   * A grid on each floor's height, a few millimeters under it so a real floor mesh wins the depth test.
   * size: square size in meters around the floor's points, or null to use the floor plan's bounds.
   */
  groundGeometries(size) {
    const result = [];
    const floors = new Set([...this.points.values()].map((p) => p.floor));
    for (const floor of floors) {
      const pts = [...this.points.values()].filter((p) => p.floor === floor);
      const y = pts.map((p) => p.floorPos.y).sort((a, b) => a - b)[Math.floor(pts.length / 2)];
      let cx, cz, w, h;
      const plan = this.floors.get(floor)?.plan;
      if (size == null && plan) {
        // Plan bounds are Unreal XY in cm; three x = UE y, three z = -UE x.
        cx = (plan.minY + plan.maxY) / 2 * CM;
        cz = -(plan.minX + plan.maxX) / 2 * CM;
        w = (plan.maxY - plan.minY) * CM;
        h = (plan.maxX - plan.minX) * CM;
      } else {
        const box = new THREE.Box3();
        for (const p of pts) box.expandByPoint(p.floorPos);
        const center = box.getCenter(new THREE.Vector3());
        cx = center.x; cz = center.z;
        w = h = size ?? 30;
      }
      // Subdivided so dollhouse grouping can give each patch of floor to its nearest point.
      const geometry = new THREE.PlaneGeometry(w, h, Math.max(1, Math.ceil(w / 1)), Math.max(1, Math.ceil(h / 1)));
      geometry.rotateX(-Math.PI / 2);
      geometry.translate(cx, y - 0.004, cz);
      result.push({ floor, geometry });
    }
    return result;
  }

  /** Other points with depth, nearest to a position: they fill spots the two main panos never saw. */
  fillPoints(position, exclude, count) {
    return [...this.points.values()]
      .filter((p) => !exclude.includes(p) && p.depthFile && p.floor === exclude[0].floor)
      .sort((a, b) => a.pos.distanceTo(position) - b.pos.distanceTo(position))
      .slice(0, count);
  }

  /** Decodes <id>_depth.png (24-bit millimeters) into meters, plus a float texture for the shader. */
  loadDepth(point) {
    if (!point.depthFile) return Promise.resolve(null);
    if (!point.depthPromise) {
      point.depthPromise = (async () => {
        const blob = await (await fetch(this.url(point.depthFile))).blob();
        // The bytes are numbers, not colors: no color management.
        const bitmap = await createImageBitmap(blob, { colorSpaceConversion: 'none', premultiplyAlpha: 'none' });
        const canvas = document.createElement('canvas');
        canvas.width = bitmap.width;
        canvas.height = bitmap.height;
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        ctx.drawImage(bitmap, 0, 0);
        const w = bitmap.width, h = bitmap.height;
        const rgba = ctx.getImageData(0, 0, w, h).data;
        bitmap.close();

        // Decode 24-bit millimeters once, then derive both copies with tight typed-array loops (no per-pixel calls).
        const n = w * h;
        const full = new Float32Array(n);
        for (let i = 0, j = 0; i < n; i++, j += 4) full[i] = (rgba[j] * 65536 + rgba[j + 1] * 256 + rgba[j + 2]) * 0.001;

        // GPU: full resolution as half floats (4 MB instead of 8 for a 2048 map) for the visibility tests.
        const bits = new Uint32Array(full.buffer);
        const halves = new Uint16Array(n);
        for (let i = 0; i < n; i++) {
          const x = bits[i];
          const exponent = ((x >>> 23) & 0xff) - 112; // rebias 127 -> 15
          halves[i] = exponent <= 0 ? 0 : exponent >= 31 ? 0x7bff : (exponent << 10) | ((x >>> 13) & 0x3ff);
        }
        const texture = new THREE.DataTexture(halves, w, h, THREE.RedFormat, THREE.HalfFloatType);
        texture.minFilter = texture.magFilter = THREE.NearestFilter;
        texture.wrapS = THREE.RepeatWrapping;
        texture.generateMipmaps = false;
        texture.needsUpdate = true;

        // CPU: half resolution is plenty for circle visibility and the cursor fallback (2 MB instead of 8).
        // Each cell keeps the nearest of its four texels, so occluders never shrink.
        const cw = w >> 1, ch = h >> 1;
        const meters = new Float32Array(cw * ch);
        for (let y = 0; y < ch; y++) {
          const r0 = 2 * y * w, r1 = r0 + w;
          for (let x = 0; x < cw; x++) {
            const a = full[r0 + 2 * x], b = full[r0 + 2 * x + 1], c = full[r1 + 2 * x], d = full[r1 + 2 * x + 1];
            let nearest = 0;
            if (a > 0) nearest = a;
            if (b > 0 && (nearest === 0 || b < nearest)) nearest = b;
            if (c > 0 && (nearest === 0 || c < nearest)) nearest = c;
            if (d > 0 && (nearest === 0 || d < nearest)) nearest = d;
            meters[y * cw + x] = nearest;
          }
        }
        point.depth = { w: cw, h: ch, data: meters, texture };
        this.queueUpload(texture, 2);
        return point.depth;
      })().catch((error) => { console.warn('No depth for', point.id, error); return null; });
    }
    return point.depthPromise;
  }

  // --- navigation ----------------------------------------------------------------------------

  async start(id) {
    const point = this.points.get(id) || this.points.values().next().value;
    this.yaw = -point.headingRad;
    await this.show(point, false);
  }

  /** Starts a navigation: any older one still loading will see it was overtaken (see navToken). */
  beginNavigation() {
    this.pendingWalk = [];
    return ++this.navToken;
  }

  async show(point, animate = true) {
    if (!point || point === this.current || this.transition || this.fly) return;
    const token = this.beginNavigation();
    const from = this.current;
    // After every await: still the latest request, nothing else moved the camera, and still in a pano view.
    const overtaken = () => token !== this.navToken || this.transition || this.fly || this.current !== from || this.mode === 'dollhouse';
    this.pendingWalk = [point];

    // Walk on whatever copy is already here (the 4K is usually prefetched); otherwise fetch the small preview first.
    await this.whileLoading('Loading', Promise.all([
      this.bestTexture(point) ? null : this.requestTier(point, point.preview ? 'preview' : 'image'),
      this.loadDepth(point),
    ]));
    if (overtaken()) return;
    if (!this.bestTexture(point)) { this.pendingWalk = []; return; }
    // Sharper copies follow; the frame loop swaps them in (full resolution once the walk is over).
    this.requestTier(point, 'medium');
    this.requestTier(point, 'image');

    if (!animate || !from) {
      this.pendingWalk = [];
      this.setCurrent(point);
      return;
    }

    // With the scene mesh, both panos are projected onto the same real geometry; two more nearby panos fill holes.
    const useMesh = !!(this.transitionMesh && from.depth && point.depth);
    let fills = [];
    if (useMesh) {
      const middle = from.pos.clone().lerp(point.pos, 0.5);
      fills = this.fillPoints(middle, [from, point], SLOTS - 2);
      this.pendingWalk = [point, ...fills];
      await Promise.all(fills.map((p) => Promise.all([this.loadDepth(p), this.requestTier(p, p.preview ? 'preview' : 'image')])));
      if (overtaken()) return;
    }
    this.pendingWalk = [];

    // Every uniform and slot write happens here, after the last await, so an overtaken call can never leave its
    // texture, center or depth in this walk. Textures are picked now too, so none of them can have been evicted.
    const texture = this.bestTexture(point, { allowFull: false, uploadedOnly: true }) || this.bestTexture(point, { allowFull: false });
    if (!texture) return;
    const fillTextures = fills.map((f) => this.bestTexture(f, { allowFull: false }));

    const u = panoMaterial.uniforms;
    u.uTexB.value = texture;
    u.uDepthB.value = point.depth?.texture || emptyDepth;
    u.uHasDepthB.value = point.depth ? 1 : 0;
    u.uCenterB.value.copy(point.pos);
    u.uHeadingB.value = point.headingRad;
    u.uFloorB.value = point.floorPos.y;
    u.uRadius.value = Math.max(4, from.pos.distanceTo(point.pos) * 1.25);

    if (useMesh) {
      const m = this.transitionMaterial;
      setSlot(m, 0, from, this.bestTexture(from), 1);
      setSlot(m, 1, point, texture, 0);
      for (let i = 0; i < SLOTS - 2; i++) {
        setSlot(m, i + 2, fills[i], fillTextures[i] || null, 0.03);
      }
      m.uniforms.uUseAngle.value = 1;
      m.uniforms.uDistanceWeight.value = 0;
      m.uniforms.uUseFallbackColor.value = 0;
    }

    // Anything this walk draws that isn't on the GPU yet goes up now, before the camera moves, so no frame of the
    // walk itself can stall. With prefetching this is normally already done.
    for (const t of [texture, point.depth?.texture, ...fillTextures, ...fills.map((f) => f.depth?.texture)]) {
      this.uploadNow(t);
    }
    const distance = from.pos.distanceTo(point.pos);
    const duration = THREE.MathUtils.clamp(TRANSITION_MIN_MS + distance * TRANSITION_MS_PER_METER, TRANSITION_MIN_MS, TRANSITION_MAX_MS);
    this.transition = { from, to: point, start: performance.now(), duration, useMesh, fills };
    this.markers.visible = false;
    this.cursor.visible = false;
  }

  /** Points the full-screen pano at a point (standing there, or the destination of a fly back from the dollhouse). */
  setPanoA(point) {
    const u = panoMaterial.uniforms;
    u.uTexA.value = this.bestTexture(point);
    u.uTexB.value = u.uTexA.value;
    u.uDepthA.value = point.depth?.texture || emptyDepth;
    u.uHasDepthA.value = point.depth ? 1 : 0;
    u.uCenterA.value.copy(point.pos);
    u.uHeadingA.value = point.headingRad;
    u.uFloorA.value = point.floorPos.y;
    u.uMix.value = 0;
  }

  setCurrent(point) {
    this.setPanoA(point);
    this.current = point;
    this.dollhouseFloor = point.floor;
    this.camera.position.copy(point.pos);
    $('point-name').textContent = `${point.id}${this.floors.size > 1 ? ' · ' + this.floorName(point.floor) : ''}`;
    history.replaceState(null, '', `#${encodeURIComponent(point.id)}`);
    this.buildMarkers();
    this.updateFloorButtons();
    this.minimap.draw();
    if (this.mode === 'plan') this.planView.draw();

    // Full resolution for where you stand. For every circle you could click: preview and depth; for the six
    // closest also the 4K walking copy. Hovering a circle bumps that point to the front (see frame()).
    this.requestTier(point, 'image', 3);
    const reachable = this.markers.children.map((c) => c.userData.point)
      .sort((a, b) => a.pos.distanceTo(point.pos) - b.pos.distanceTo(point.pos));
    reachable.forEach((n, i) => {
      this.loadDepth(n).then((depth) => depth && this.queueUpload(depth.texture, 2));
      this.requestTier(n, n.preview ? 'preview' : 'image', 2).then(() => {
        if (i < 6) this.requestTier(n, 'medium', 1.5);
      });
    });
    this.evictTextures();
  }

  goToFloor(index) {
    if (this.mode === 'dollhouse') {
      this.dollhouseFloor = index;
      this.updateDollhouseVisibility();
      this.updateFloorButtons();
      return;
    }
    if (this.current?.floor === index) return;
    const target = [...this.points.values()].find((p) => p.floor === index);
    if (target) this.show(target, false);
  }

  floorName(index) { return this.floors.get(index)?.name || `Floor ${index + 1}`; }

  // --- circles -------------------------------------------------------------------------------

  makeCircle(radius, opacity) {
    // One geometry per size, shared by every circle; each circle has its own material (hover changes its opacity).
    this.circleGeometries ??= new Map();
    if (!this.circleGeometries.has(radius)) this.circleGeometries.set(radius, new THREE.PlaneGeometry(radius * CIRCLE_QUAD, radius * CIRCLE_QUAD));
    const mesh = new THREE.Mesh(this.circleGeometries.get(radius), makeCircleMaterial(opacity));
    mesh.renderOrder = 2;
    return mesh;
  }

  /** Circles lie flat on the floor, facing up. */
  makeFloorCircle(radius, opacity) {
    const mesh = this.makeCircle(radius, opacity);
    mesh.rotation.x = -Math.PI / 2;
    return mesh;
  }

  /**
   * Circles all the way around: in every direction the nearest visible point gets a circle, however far away it is.
   * The next point behind it (within MARKER_SECTOR_DEG, as seen from here) is drawn faded, and anything further
   * back in that direction is left out, so the floor never fills up with a row of circles.
   */
  buildMarkers() {
    // Rebuilt on every arrival: free the old circles' materials (the geometry is shared and stays).
    for (const circle of this.markers.children) circle.material.dispose();
    this.markers.clear();

    const here = this.current;
    const candidates = [];
    for (const p of this.points.values()) {
      if (p === here || p.floor !== here.floor) continue;
      // Hide circles behind walls, like Matterport: the target spot has to be visible from here.
      const spot = new THREE.Vector3(p.floorPos.x, here.floorPos.y + 0.05, p.floorPos.z);
      if (here.depth && !sees(here, spot, 0.3)) continue;
      const dx = spot.x - here.pos.x, dz = spot.z - here.pos.z;
      candidates.push({ p, spot, distance: Math.hypot(dx, dz), azimuth: Math.atan2(dx, -dz) });
    }
    candidates.sort((a, b) => a.distance - b.distance);

    const sector = THREE.MathUtils.degToRad(MARKER_SECTOR_DEG);
    const placed = [];
    for (const c of candidates) {
      // How many closer circles already sit in this direction: 0 = the nearest there, 1 = the one behind it.
      const inFront = placed.filter((q) => Math.abs(Math.atan2(Math.sin(c.azimuth - q.azimuth), Math.cos(c.azimuth - q.azimuth))) < sector).length;
      if (inFront >= 2) continue;
      placed.push(c);

      const opacity = inFront === 0 ? MARKER_OPACITY : MARKER_OPACITY_BEHIND;
      const circle = this.makeFloorCircle(MARKER_RADIUS, opacity);
      circle.position.set(c.spot.x, here.floorPos.y + 0.02, c.spot.z);
      // Far circles grow with distance so they stay big enough to see and click; near ones keep Matterport's size.
      circle.userData.baseScale = THREE.MathUtils.clamp(c.distance / MARKER_GROW_FROM, 1, MARKER_MAX_SCALE);
      circle.userData.baseOpacity = opacity;
      circle.scale.setScalar(circle.userData.baseScale);
      circle.userData.point = c.p;
      this.markers.add(circle);
    }
    this.markers.visible = this.mode === 'pano';
  }

  // --- dollhouse -----------------------------------------------------------------------------

  /** Built once, even when the dollhouse is asked for again while it is still building. */
  ensureDollhouse() {
    // No text here: buildDollhouse shows its own progress.
    this.dollhousePromise ??= this.whileLoading(null, this.buildDollhouse()).catch((error) => {
      console.warn('Could not build the dollhouse', error);
      this.dollhousePromise = null;
      return null;
    });
    return this.dollhousePromise;
  }

  async buildDollhouse() {
    const points = [...this.points.values()].filter((p) => p.depthFile);
    let done = 0;
    this.setLoading(true, `Building dollhouse 0/${points.length}`);
    await Promise.all(points.map(async (p) => {
      await Promise.all([this.loadDepth(p), this.dollhouseTexture(p)]);
      this.setLoading(true, `Building dollhouse ${++done}/${points.length}`);
    }));

    await this.meshReady;
    const group = this.proxyGeometry
      ? await this.buildMeshDollhouse(points.filter((p) => p.depth))
      : new THREE.Group();
    if (!this.proxyGeometry) {
      for (const p of points) {
        if (!p.depth) continue;
        const texture = await this.dollhouseTexture(p);
        const mesh = this.buildPanoMesh(p, points, texture);
        if (mesh) group.add(mesh);
      }
    }
    for (const p of this.points.values()) {
      const circle = this.makeFloorCircle(MARKER_RADIUS * 1.3, 0.9);
      circle.position.set(p.floorPos.x, p.floorPos.y + 0.03, p.floorPos.z);
      circle.userData.point = p;
      this.dollhouseMarkers.add(circle);
    }
    this.scene.add(group);
    this.dollhouse = group;
    // Upload everything the dollhouse draws while the loading indicator is still up, so the fly-in doesn't stall.
    for (const p of points) {
      this.uploadNow(await this.dollhouseTexture(p));
      this.uploadNow(p.depth?.texture);
    }
    this.dollhouse.visible = true; // compile() only visits visible objects; frame() sets visibility again
    this.renderer.compile(this.scene, this.camera);
    return group;
  }

  /**
   * Dollhouse from the real scene mesh: triangles are grouped by their nearest capture point, and each group
   * is textured by that point and its neighbors, per pixel picking the closest pano that actually sees it.
   * Ceilings and anything above the cut height are removed so you can look in from above.
   */
  async buildMeshDollhouse(points) {
    const sources = [this.proxyGeometry, ...this.groundGeometries(null).map((g) => g.geometry)];
    const group = new THREE.Group();
    for (const geometry of sources) {
      await this.addDollhouseSource(group, geometry, points);
    }
    return group;
  }

  async addDollhouseSource(group, geometry, points) {
    const position = geometry.getAttribute('position');
    const index = geometry.index.array;
    const groups = new Map();
    const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3(), centroid = new THREE.Vector3();
    const e1 = new THREE.Vector3(), e2 = new THREE.Vector3(), normal = new THREE.Vector3();
    const nearest = [];

    for (let t = 0; t < index.length; t += 3) {
      a.fromBufferAttribute(position, index[t]);
      b.fromBufferAttribute(position, index[t + 1]);
      c.fromBufferAttribute(position, index[t + 2]);
      centroid.copy(a).add(b).add(c).divideScalar(3);

      // The four nearest points (mostly horizontal distance, preferring the floor the triangle sits on).
      // Grouping by that whole set, not just the nearest, keeps the blend continuous: crossing into the next
      // group only swaps the 4th-nearest pano, whose weight is already tiny.
      nearest.length = 0;
      for (const p of points) {
        const dy = centroid.y - p.floorPos.y;
        const vertical = dy < -0.3 ? 10 : dy > DOLLHOUSE_CUT + 0.5 ? 10 : 0;
        const d = Math.hypot(centroid.x - p.pos.x, centroid.z - p.pos.z) + vertical;
        if (nearest.length < SLOTS || d < nearest[nearest.length - 1].d) {
          nearest.push({ p, d });
          nearest.sort((x, y) => x.d - y.d);
          if (nearest.length > SLOTS) nearest.pop();
        }
      }
      const owner = nearest[0]?.p;
      if (!owner || nearest[0].d > DOLLHOUSE_RANGE) continue;
      // Entirely above the cut: skip. Straddling it: keep, the shader clips the rest.
      if (Math.min(a.y, b.y, c.y) > owner.floorPos.y + DOLLHOUSE_CUT) continue;
      normal.crossVectors(e1.subVectors(b, a), e2.subVectors(c, a));
      if (normal.lengthSq() < 1e-12) continue;
      normal.normalize();
      if (normal.y < -0.7 && centroid.y > owner.floorPos.y + 1.2) continue; // ceilings seen from below

      const key = nearest.map((n) => n.p.id).sort().join('|');
      if (!groups.has(key)) groups.set(key, { owner, slots: nearest.map((n) => n.p), indices: [] });
      groups.get(key).indices.push(index[t], index[t + 1], index[t + 2]);
    }

    for (const { owner, slots, indices } of groups.values()) {
      const material = makeProjectiveMaterial();
      material.uniforms.uUseAngle.value = 0;
      material.uniforms.uDistanceWeight.value = 1;
      material.uniforms.uUseFallbackColor.value = 1;
      material.uniforms.uClipY.value = owner.floorPos.y + DOLLHOUSE_CUT;
      for (let i = 0; i < SLOTS; i++) {
        const p = slots[i];
        setSlot(material, i, p, p ? await this.dollhouseTexture(p) : null, 1);
      }
      const part = new THREE.BufferGeometry();
      part.setAttribute('position', position);
      part.setIndex(indices);
      part.computeBoundingSphere();
      const mesh = new THREE.Mesh(part, material);
      mesh.userData.floor = owner.floor;
      group.add(mesh);
    }
  }

  /**
   * Turns one pano's depth into a textured mesh. Each surface is kept only by the nearest capture point
   * that can see it, so overlapping scans don't double up, and everything above the cut height is removed.
   */
  buildPanoMesh(point, allPoints, texture) {
    const { w, h, data } = point.depth;
    const nx = Math.min(DOLLHOUSE_GRID, w);
    const ny = Math.round(nx / 2);
    const verts = (nx + 1) * (ny + 1);
    const positions = new Float32Array(verts * 3);
    const uvs = new Float32Array(verts * 2);
    const distances = new Float32Array(verts);
    const c = Math.cos(point.headingRad), s = Math.sin(point.headingRad);

    for (let j = 0; j <= ny; j++) {
      const v = j / ny;
      const lat = (0.5 - v) * Math.PI;
      const py = Math.min(Math.floor(v * h), h - 1);
      for (let i = 0; i <= nx; i++) {
        const u = i / nx;
        const lon = (u - 0.5) * 2 * Math.PI;
        const lx = Math.cos(lat) * Math.cos(lon), ly = Math.cos(lat) * Math.sin(lon), lz = Math.sin(lat);
        const wx = c * lx - s * ly, wy = s * lx + c * ly;          // pano frame -> Unreal world
        const px = Math.floor(u * w) % w;
        const d = data[py * w + px];
        const k = j * (nx + 1) + i;
        distances[k] = d > 0 && d < DOLLHOUSE_RANGE ? d : 0;
        positions[k * 3] = point.pos.x + wy * d;                  // Unreal -> three
        positions[k * 3 + 1] = point.pos.y + lz * d;
        positions[k * 3 + 2] = point.pos.z - wx * d;
        uvs[k * 2] = u;
        uvs[k * 2 + 1] = v;
      }
    }

    const others = allPoints.filter((q) => q !== point && q.depth && q.floor === point.floor && q.pos.distanceTo(point.pos) < DOLLHOUSE_RANGE * 2);
    const cutY = point.floorPos.y + DOLLHOUSE_CUT;
    const indices = [];
    const a = new THREE.Vector3(), b = new THREE.Vector3(), cc = new THREE.Vector3(), centroid = new THREE.Vector3();
    const e1 = new THREE.Vector3(), e2 = new THREE.Vector3(), normal = new THREE.Vector3(), view = new THREE.Vector3();

    const keep = (i0, i1, i2) => {
      const d0 = distances[i0], d1 = distances[i1], d2 = distances[i2];
      if (!d0 || !d1 || !d2) return;
      a.fromArray(positions, i0 * 3); b.fromArray(positions, i1 * 3); cc.fromArray(positions, i2 * 3);
      centroid.copy(a).add(b).add(cc).divideScalar(3);
      if (centroid.y > cutY) return;
      normal.crossVectors(e1.subVectors(b, a), e2.subVectors(cc, a)).normalize();
      // Triangles the camera ray skims along are the "skin" stretched across a depth edge, not real surfaces.
      // Real surfaces seen at a shallow angle (distant floor) still face the camera enough to pass.
      view.subVectors(centroid, point.pos).normalize();
      const facing = Math.abs(normal.dot(view));
      const lo = Math.min(d0, d1, d2), hi = Math.max(d0, d1, d2);
      const jump = hi / lo > 1.3 && hi - lo > 0.15; // distant floor rows differ by less than this
      if (facing < 0.07 || (jump && facing < 0.3)) return;
      if (Math.abs(normal.y) > 0.7 && centroid.y > point.floorPos.y + 1.2) return; // ceilings, table tops seen from below
      const own = centroid.distanceTo(point.pos);
      for (const q of others) {
        if (centroid.distanceTo(q.pos) < own - 0.05 && sees(q, centroid)) return;
      }
      indices.push(i0, i1, i2);
    };

    for (let j = 0; j < ny; j++) {
      for (let i = 0; i < nx; i++) {
        const k = j * (nx + 1) + i;
        keep(k, k + nx + 1, k + 1);
        keep(k + 1, k + nx + 1, k + nx + 2);
      }
    }
    if (!indices.length) return null;

    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    geometry.setAttribute('uv', new THREE.BufferAttribute(uvs, 2));
    geometry.setIndex(indices);
    geometry.computeBoundingSphere();
    const mesh = new THREE.Mesh(geometry, new THREE.MeshBasicMaterial({ map: texture, side: THREE.DoubleSide }));
    mesh.userData.floor = point.floor;
    return mesh;
  }

  updateDollhouseVisibility() {
    if (!this.dollhouse) return;
    // Show the selected floor and everything below it, like Matterport's floor stack.
    for (const mesh of this.dollhouse.children) mesh.visible = mesh.userData.floor <= this.dollhouseFloor;
    for (const circle of this.dollhouseMarkers.children) circle.visible = circle.userData.point.floor === this.dollhouseFloor;
  }

  dollhouseView() {
    const pts = [...this.points.values()].filter((p) => p.floor === this.dollhouseFloor);
    const box = new THREE.Box3();
    for (const p of pts) box.expandByPoint(p.floorPos);
    const center = box.getCenter(new THREE.Vector3());
    // Frame the floor's footprint (plus a margin for the walls around the outer points) to fill the view.
    const radius = Math.max(4, box.getSize(new THREE.Vector3()).length() / 2 + 2);
    const halfFov = Math.min(THREE.MathUtils.degToRad(this.camera.fov), this.horizontalFov()) / 2;
    const distance = radius / Math.sin(halfFov) * 1.05;
    const heading = -this.yaw;
    // Look at the model from behind the current view direction, about 45 degrees from above.
    const back = new THREE.Vector3(Math.sin(heading), 0, Math.cos(heading));
    const direction = back.multiplyScalar(Math.SQRT1_2).add(new THREE.Vector3(0, Math.SQRT1_2, 0));
    return { target: center, position: center.clone().addScaledVector(direction, distance) };
  }

  async enterDollhouse() {
    if (this.transition || this.fly) return;
    const token = this.beginNavigation();
    const dollhouse = await this.ensureDollhouse();
    // A walk may have started while it was building (the first build takes a while); that walk wins.
    if (!dollhouse || token !== this.navToken || this.transition || this.fly || this.mode === 'dollhouse') return;
    this.mode = 'dollhouse';
    this.updateModeUi();
    this.updateDollhouseVisibility();
    const { target, position } = this.dollhouseView();
    // The photo stays on top and dissolves into the model during the first half of the pull-out.
    this.startFly(this.camera.position.clone(), position, this.camera.getWorldDirection(new THREE.Vector3()).add(this.camera.position), target, () => {
      this.controls.target.copy(target);
      this.controls.enabled = true;
      this.controls.update();
    }, { duration: DOLLHOUSE_FLY_MS, panoFade: 'out', panoPoint: this.current });
  }

  async flyToPano(point) {
    if (this.fly || this.transition || !point) return;
    const token = this.beginNavigation();
    this.pendingWalk = [point];
    this.controls.enabled = false;
    await this.whileLoading('Loading', Promise.all([
      this.bestTexture(point) ? null : this.requestTier(point, point.preview ? 'preview' : 'image'),
      this.loadDepth(point),
    ]));
    if (token !== this.navToken || this.fly || this.mode !== 'dollhouse') {
      if (this.mode === 'dollhouse' && !this.fly) this.controls.enabled = true;
      return;
    }
    this.pendingWalk = [];
    this.requestTier(point, 'image');
    // Keep facing the way the dollhouse camera looked, levelled.
    const forward = this.camera.getWorldDirection(new THREE.Vector3());
    forward.y = 0;
    if (!isFiniteVector(forward) || forward.lengthSq() < 1e-6) forward.set(-Math.sin(this.yaw) || 0, 0, -Math.cos(this.yaw) || -1);
    forward.normalize();
    const arrive = () => {
      // Snap exactly onto the point in a clean pano state, whatever the fly left behind.
      this.fly = null;
      this.yaw = Math.atan2(-forward.x, -forward.z);
      this.pitch = 0;
      this.mode = 'pano';
      this.current = null;
      this.setCurrent(point);
      this.updateModeUi();
    };
    const fromPos = this.camera.position.clone();
    const fromTarget = this.controls.target.clone();
    if (!isFiniteVector(fromPos) || !isFiniteVector(fromTarget)) {
      arrive(); // nothing sensible to fly from
      return;
    }
    // The destination's photo fades in over the model during the second half, so arriving is seamless.
    this.setPanoA(point);
    this.startFly(fromPos, point.pos.clone(), fromTarget, point.pos.clone().add(forward), arrive,
      { duration: DOLLHOUSE_FLY_MS, panoFade: 'in', panoPoint: point });
  }

  /** Back to 360 from the dollhouse: the point we left from, or on another floor the one nearest the model's centre. */
  exitDollhouse() {
    const target = this.current?.floor === this.dollhouseFloor
      ? this.current
      : this.nearestPoint(this.controls.target, this.dollhouseFloor) || this.current;
    return this.flyToPano(target);
  }

  /**
   * panoFade: 'out' (leaving a point) or 'in' (arriving at one) dissolves the full-screen pano of panoPoint over the
   * model. The pano is depth-traced from the moving camera, so it stays in place while it fades.
   */
  startFly(fromPos, toPos, fromLook, toLook, onDone, { duration = FLY_MS, panoFade = null, panoPoint = null } = {}) {
    this.fly = { fromPos, toPos, fromLook, toLook, onDone, duration, panoFade, panoPoint, progress: 0, start: performance.now() };
  }

  /** Opacity of the full-screen pano during a fly: fully there at the point, gone half way out. */
  flyPanoOpacity() {
    const fly = this.fly;
    if (!fly?.panoFade) return 0;
    const t = fly.progress;
    return fly.panoFade === 'out' ? 1 - THREE.MathUtils.smoothstep(t, 0, 0.5) : THREE.MathUtils.smoothstep(t, 0.5, 1);
  }

  // --- input ---------------------------------------------------------------------------------

  bindInput() {
    const canvas = this.renderer.domElement;
    let drag = null;

    canvas.addEventListener('pointerdown', (e) => {
      // Taps have no pointermove before them, so aim from here.
      this.updatePointer(e);
      drag = { x: e.clientX, y: e.clientY, moved: 0 };
      if (this.mode === 'pano') {
        canvas.setPointerCapture(e.pointerId);
        $('stage').classList.add('dragging');
      }
    });

    canvas.addEventListener('pointermove', (e) => {
      this.updatePointer(e);
      // Only moving the view counts: hovering alone used to hold back the full-resolution upload indefinitely,
      // so the view stayed on the 4K copy for as long as the mouse kept moving.
      if (!drag) return;
      this.lastInteraction = performance.now();
      const dx = e.clientX - drag.x;
      const dy = e.clientY - drag.y;
      drag.moved += Math.abs(dx) + Math.abs(dy);
      drag.x = e.clientX;
      drag.y = e.clientY;
      this.dragging = drag.moved > 6;
      if (this.mode !== 'pano') return; // OrbitControls handles the dollhouse
      // Grab-to-pan: the view follows the pointer at a rate that matches the current zoom.
      const radiansPerPixel = THREE.MathUtils.degToRad(this.camera.fov) / canvas.clientHeight;
      this.yaw += dx * radiansPerPixel;
      this.pitch = THREE.MathUtils.clamp(this.pitch + dy * radiansPerPixel, -1.45, 1.45);
    });

    const end = () => {
      $('stage').classList.remove('dragging');
      if (drag && drag.moved < 6) this.click();
      drag = null;
      this.dragging = false;
    };
    canvas.addEventListener('pointerup', end);
    // A drag the browser takes over or loses (touch gesture, focus change) must end too: a stuck `dragging` hid the
    // cursor and held back every texture upload, leaving the view on its low-resolution copy.
    const cancel = () => {
      drag = null;
      this.dragging = false;
      $('stage').classList.remove('dragging');
    };
    canvas.addEventListener('pointercancel', cancel);
    canvas.addEventListener('lostpointercapture', () => { if (drag) cancel(); });
    window.addEventListener('blur', cancel);

    canvas.addEventListener('wheel', (e) => {
      this.lastInteraction = performance.now();
      if (this.mode !== 'pano') return;
      e.preventDefault();
      this.zoom = THREE.MathUtils.clamp(this.zoom * Math.exp(e.deltaY * 0.001), ZOOM_MIN, ZOOM_MAX);
      this.updateFov();
    }, { passive: false });

    window.addEventListener('keydown', (e) => {
      this.lastInteraction = performance.now();
      const step = 0.08;
      if (e.key === 'ArrowLeft') this.yaw += step;
      else if (e.key === 'ArrowRight') this.yaw -= step;
      else if (e.key === 'ArrowUp') this.pitch = Math.min(this.pitch + step, 1.45);
      else if (e.key === 'ArrowDown') this.pitch = Math.max(this.pitch - step, -1.45);
      else if (e.key === 'f' || e.key === 'F') toggleFullscreen();
      else if (e.key === '1') this.setMode('pano');
      else if (e.key === '2') this.setMode('dollhouse');
      else if (e.key === '3') this.setMode('plan');
    });

    $('btn-pano').onclick = () => this.setMode('pano');
    $('btn-dollhouse').onclick = () => this.setMode('dollhouse');
    $('btn-plan').onclick = () => this.setMode('plan');
    $('btn-fullscreen').onclick = toggleFullscreen;
  }

  updatePointer(e) {
    const rect = this.renderer.domElement.getBoundingClientRect();
    this.pointer.set(((e.clientX - rect.left) / rect.width) * 2 - 1, -((e.clientY - rect.top) / rect.height) * 2 + 1);
    this.pointerActive = true;
  }

  /**
   * Surface under the pointer in pano mode. The exported mesh gives exact positions and clean face normals;
   * where something that isn't in the mesh (foliage, characters) sits in front, the pano's depth takes over.
   * Normals snap flat on floors and upright on walls, like Matterport's cursor.
   */
  surfaceHit() {
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const ray = this.raycaster.ray;
    const p = this.current;
    const depth = p.depth ? depthAlongSmooth(p, ray.direction) : 0;
    let hit = null;
    let normal = null;

    if (this.cursorTargets) {
      this.raycaster.firstHitOnly = true;
      const hits = [];
      for (const mesh of this.cursorTargets) mesh.raycast(this.raycaster, hits);
      hits.sort((x, y) => x.distance - y.distance);
      const first = hits[0];
      if (first && first.distance < 40) {
        if (depth > 0 && depth < first.distance - Math.max(0.15, first.distance * 0.05)) {
          hit = ray.at(depth, new THREE.Vector3());
        } else {
          hit = first.point.clone();
          normal = first.face.normal.clone();
        }
      }
    }

    if (!hit && depth > 0 && depth < 30) {
      hit = ray.at(depth, new THREE.Vector3());
    }
    if (hit && !normal && p.depth) {
      // Normal from the depth map, over a wide enough baseline (~1.7 degrees) to average out texel steps.
      const dir = ray.direction;
      const side = new THREE.Vector3().crossVectors(dir, new THREE.Vector3(0, 1, 0));
      if (side.lengthSq() < 1e-6) side.set(1, 0, 0);
      side.normalize();
      const up = new THREE.Vector3().crossVectors(side, dir).normalize();
      const eps = 0.03;
      const sample = (offset) => {
        const d2 = dir.clone().add(offset).normalize();
        const t = depthAlongSmooth(p, d2);
        return t ? p.pos.clone().addScaledVector(d2, t) : null;
      };
      const h1 = sample(side.clone().multiplyScalar(eps)), h2 = sample(side.clone().multiplyScalar(-eps));
      const h3 = sample(up.clone().multiplyScalar(eps)), h4 = sample(up.clone().multiplyScalar(-eps));
      if (h1 && h2 && h3 && h4) normal = new THREE.Vector3().crossVectors(h1.sub(h2), h3.sub(h4)).normalize();
    }
    if (!hit) {
      if (ray.direction.y > -0.02) return null;
      const t = (p.floorPos.y - ray.origin.y) / ray.direction.y;
      if (!(t > 0 && t < 25)) return null;
      hit = ray.at(t, new THREE.Vector3());
      normal = new THREE.Vector3(0, 1, 0);
    }
    if (!normal || !Number.isFinite(normal.x)) normal = ray.direction.clone().negate();
    if (normal.dot(ray.direction) > 0) normal.negate();

    if (normal.y > 0.8) normal.set(0, 1, 0);
    else if (Math.abs(normal.y) < 0.25) { normal.y = 0; normal.normalize(); }
    return { hit, normal };
  }

  hoveredMarker(group) {
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const hits = this.raycaster.intersectObjects(group.children.filter((c) => c.visible), false);
    return hits.length ? hits[0].object.userData.point : null;
  }

  click() {
    if (this.transition || this.fly) return;

    if (this.mode === 'dollhouse') {
      let target = this.hoveredMarker(this.dollhouseMarkers);
      if (!target && this.dollhouse) {
        // Clicking the model flies to the nearest capture point to where you clicked.
        this.raycaster.setFromCamera(this.pointer, this.camera);
        const hit = this.raycaster.intersectObjects(this.dollhouse.children.filter((m) => m.visible), false)[0];
        if (hit) target = this.nearestPoint(hit.point, this.dollhouseFloor);
      }
      if (target) this.flyToPano(target);
      return;
    }

    if (this.mode !== 'pano' || !this.current) return;
    const marker = this.hoveredMarker(this.markers);
    if (marker) return this.show(marker);

    // Clicking a surface goes to the closest visible point near where you clicked, like Matterport.
    const surface = this.surfaceHit();
    if (!surface) return;
    const target = this.nearestPoint(surface.hit, this.current.floor, (p) => p !== this.current && (!this.current.depth || sees(this.current, new THREE.Vector3(p.floorPos.x, this.current.floorPos.y + 0.05, p.floorPos.z), 0.3)));
    if (target && target.floorPos.distanceTo(surface.hit) < 6) this.show(target);
  }

  nearestPoint(position, floor, filter = () => true) {
    let best = null;
    let bestDistance = Infinity;
    for (const p of this.points.values()) {
      if (p.floor !== floor || !filter(p)) continue;
      const d = Math.hypot(p.floorPos.x - position.x, p.floorPos.z - position.z);
      if (d < bestDistance) { best = p; bestDistance = d; }
    }
    return best;
  }

  async setMode(mode) {
    if (mode === this.mode || this.transition || this.fly) return;
    if (mode === 'dollhouse') return this.enterDollhouse();
    if (this.mode === 'dollhouse' && mode === 'pano') {
      return this.exitDollhouse();
    }
    this.mode = mode;
    this.updateModeUi();
    if (mode === 'plan') {
      this.planView.resize();
      this.planView.draw();
    }
  }

  updateModeUi() {
    const mode = this.mode;
    $('btn-pano').classList.toggle('active', mode === 'pano');
    $('btn-dollhouse').classList.toggle('active', mode === 'dollhouse');
    $('btn-plan').classList.toggle('active', mode === 'plan');
    $('plan-overlay').hidden = mode !== 'plan';
    $('minimap').hidden = mode !== 'pano' || !this.hasPlans();
    this.markers.visible = mode === 'pano';
    this.cursor.visible = false;
    this.dollhouseMarkers.visible = mode === 'dollhouse';
    if (this.dollhouse) this.dollhouse.visible = mode === 'dollhouse';
    this.controls.enabled = false;
    this.updateFloorButtons();
    // The minimap can't measure itself while hidden, so size it again once it is back.
    if (!$('minimap').hidden) {
      this.minimap.resize();
      this.minimap.draw();
    }
  }

  hasPlans() { return [...this.floors.values()].some((f) => f.plan); }

  buildFloorButtons() {
    const container = $('floors');
    container.innerHTML = '';
    const floorIndices = [...new Set([...this.points.values()].map((p) => p.floor))].sort((a, b) => a - b);
    if (floorIndices.length < 2) return;
    for (const index of floorIndices) {
      const button = document.createElement('button');
      button.textContent = this.floorName(index);
      button.dataset.floor = index;
      button.onclick = () => this.goToFloor(index);
      container.appendChild(button);
    }
  }

  updateFloorButtons() {
    const active = this.mode === 'dollhouse' ? this.dollhouseFloor : this.current?.floor;
    for (const button of $('floors').children) {
      button.classList.toggle('active', Number(button.dataset.floor) === active);
    }
  }

  setLoading(on, text) {
    $('loading').hidden = !on;
    if (text) $('loading-text').textContent = text;
  }

  /** Shows the loading indicator while a promise runs. Counted, so one load finishing doesn't hide another's. */
  async whileLoading(text, promise) {
    this.loadingCount++;
    this.setLoading(true, text);
    try {
      return await promise;
    } finally {
      if (--this.loadingCount <= 0) {
        this.loadingCount = 0;
        this.setLoading(false);
      }
    }
  }

  /** Vertical FOV from the screen shape and the scroll zoom (see VIEW_HFOV). */
  updateFov() {
    const tanHalf = (deg) => Math.tan(THREE.MathUtils.degToRad(deg) / 2);
    const base = Math.min(tanHalf(VIEW_HFOV) / this.camera.aspect, tanHalf(VIEW_MAX_VFOV));
    this.camera.fov = THREE.MathUtils.radToDeg(2 * Math.atan(base * this.zoom));
    this.camera.updateProjectionMatrix();
  }

  /** Horizontal FOV in radians, for the minimap's view cone. */
  horizontalFov() {
    return 2 * Math.atan(Math.tan(THREE.MathUtils.degToRad(this.camera.fov) / 2) * this.camera.aspect);
  }

  resize() {
    const w = window.innerWidth;
    const h = window.innerHeight;
    // A hidden or minimized window can report 0 x 0: the aspect would be NaN, and a camera placed from it (the
    // dollhouse view, the fly back to a point) stays NaN for good. Keep the last real size instead.
    if (!(w > 0 && h > 0)) return;
    this.renderer.setSize(w, h);
    this.camera.aspect = w / h;
    this.updateFov();
    $('minimap').hidden = this.mode !== 'pano' || !this.hasPlans();
    this.minimap.resize();
    this.planView.resize();
    this.minimap.draw();
    if (this.mode === 'plan') this.planView.draw();
  }

  // --- frame ---------------------------------------------------------------------------------

  frame() {
    const u = panoMaterial.uniforms;
    const inPano = this.mode === 'pano' || this.mode === 'plan';

    if (this.fly) {
      const t = Math.min((performance.now() - this.fly.start) / this.fly.duration, 1);
      this.fly.progress = t;
      const e = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
      this.camera.position.lerpVectors(this.fly.fromPos, this.fly.toPos, e);
      this.camera.lookAt(new THREE.Vector3().lerpVectors(this.fly.fromLook, this.fly.toLook, e));
      if (t >= 1) {
        const done = this.fly.onDone;
        this.fly = null;
        done?.();
      }
    } else if (this.mode === 'dollhouse') {
      this.controls.update();
    }

    this.pumpUploads(performance.now());

    // Swap in sharper copies once they're on the GPU (the upload queue gets them there while the view is idle).
    // Flying back from the dollhouse, the pano on screen is the destination's, not the current point's.
    const panoPoint = this.fly?.panoPoint || this.current;
    if (panoPoint && !this.transition) {
      const best = this.bestTexture(panoPoint, { uploadedOnly: true }) || this.bestTexture(panoPoint);
      if (best && u.uTexA.value !== best) u.uTexA.value = best;
    } else if (this.transition) {
      const next = this.bestTexture(this.transition.to, { allowFull: false, uploadedOnly: true });
      if (next && u.uTexB.value !== next) {
        u.uTexB.value = next;
        this.transitionMaterial.uniforms.uTex1.value = next;
      }
    }

    if (this.transition) {
      const t = Math.min((performance.now() - this.transition.start) / this.transition.duration, 1);
      // Cubic ease: gentle start and stop, and the crossfade happens while moving fastest.
      const eased = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
      this.camera.position.lerpVectors(this.transition.from.pos, this.transition.to.pos, eased);
      const mix = THREE.MathUtils.smoothstep(eased, 0.15, 0.85);
      u.uMix.value = mix;
      if (this.transition.useMesh) {
        const w = this.transitionMaterial.uniforms.uWeight.value;
        w[0] = 1 - mix;
        w[1] = mix;
        this.transitionMaterial.uniforms.uCamPos.value.copy(this.camera.position);
      }
      if (t >= 1) {
        const { to } = this.transition;
        this.transition = null;
        this.setCurrent(to);
      }
    }

    // Pano mode drives the camera from yaw/pitch; the dollhouse and fly-ins set it directly.
    const showPano = inPano && !this.fly;
    if (showPano) this.camera.rotation.set(this.pitch, this.yaw, 0);
    this.camera.updateMatrixWorld();
    const meshWalk = showPano && !!this.transition?.useMesh;
    // While walking on the mesh, the sky sphere replaces the full-screen pano entirely. Flying to or from the
    // dollhouse, the pano dissolves over the model instead of switching off.
    const flyPano = this.flyPanoOpacity();
    this.panoMesh.visible = (showPano && !meshWalk) || flyPano > 0.002;
    u.uOpacity.value = showPano ? 1 : flyPano;
    if (this.transitionMesh) this.transitionMesh.visible = meshWalk;
    if (meshWalk) this.skySphere.position.copy(this.camera.position);
    if (this.dollhouse) this.dollhouse.visible = !showPano;
    this.dollhouseMarkers.visible = this.mode === 'dollhouse' && !this.fly;

    // Hover feedback: grow the circle under the pointer, or show the surface cursor.
    const now = performance.now();
    const dt = Math.min((now - (this.lastFrame ?? now)) / 1000, 0.1);
    this.lastFrame = now;
    if (this.current && !this.transition && this.mode === 'pano' && this.pointerActive && showPano) {
      const marker = this.hoveredMarker(this.markers);
      if (marker && marker !== this.hoverPrefetched) {
        // Hover intent: this is probably the next click, so its walk data jumps the queue.
        this.hoverPrefetched = marker;
        this.prefetchForWalk(marker, 5, { full: true });
      }
      for (const circle of this.markers.children) {
        const hot = circle.userData.point === marker;
        circle.scale.setScalar(circle.userData.baseScale * (hot ? 1.2 : 1));
        circle.material.uniforms.uOpacity.value = hot ? 1 : circle.userData.baseOpacity;
      }
      // Hidden while dragging the view, like Matterport; otherwise it glides to the surface instead of jumping.
      const surface = marker || this.dragging ? null : this.surfaceHit();
      if (surface) {
        const targetPos = surface.hit.clone().addScaledVector(surface.normal, 0.015);
        const targetRot = new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), surface.normal);
        if (!this.cursor.visible) {
          this.cursor.position.copy(targetPos);
          this.cursor.quaternion.copy(targetRot);
        } else {
          this.cursor.position.lerp(targetPos, 1 - Math.exp(-dt * 30));
          this.cursor.quaternion.slerp(targetRot, 1 - Math.exp(-dt * 18));
        }
      }
      this.cursor.visible = !!surface;
      $('stage').classList.toggle('pointing', !!marker);
    } else if (this.mode === 'dollhouse' && this.pointerActive && !this.fly) {
      const marker = this.hoveredMarker(this.dollhouseMarkers);
      for (const circle of this.dollhouseMarkers.children) circle.scale.setScalar(circle.userData.point === marker ? 1.25 : 1);
      $('stage').classList.toggle('pointing', !!marker);
    }

    u.uCamPos.value.copy(this.camera.position);
    u.uInvProj.value.copy(this.camera.projectionMatrixInverse);
    u.uCamWorld.value.copy(this.camera.matrixWorld);
    this.renderer.render(this.scene, this.camera);

    // The minimap's view cone follows the camera.
    if (this.lastYaw !== this.yaw || this.transition) {
      this.lastYaw = this.yaw;
      this.minimap.draw();
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Top-down floor plan with capture points, used for the minimap and the full plan view.

class PlanView {
  constructor(canvas, viewer, { compact }) {
    this.canvas = canvas;
    this.viewer = viewer;
    this.compact = compact;
    this.images = new Map();
    this.layout = null;
    canvas.addEventListener('click', (e) => this.click(e));
  }

  resize() {
    const ratio = Math.min(window.devicePixelRatio, 2);
    this.canvas.width = Math.max(1, Math.round(this.canvas.clientWidth * ratio));
    this.canvas.height = Math.max(1, Math.round(this.canvas.clientHeight * ratio));
    this.ratio = ratio;
  }

  planFor(floor) { return this.viewer.floors.get(floor)?.plan; }

  image(plan) {
    if (!this.images.has(plan.image)) {
      const img = new Image();
      img.onload = () => this.draw();
      img.src = this.viewer.url(plan.image);
      this.images.set(plan.image, img);
    }
    return this.images.get(plan.image);
  }

  /** Unreal XY (cm) -> canvas pixels. Plans have +Y to the right and +X up. */
  project(x, y) {
    const { plan, ox, oy, scale } = this.layout;
    return [ox + (y - plan.minY) * scale, oy + (plan.maxX - x) * scale];
  }

  draw() {
    const viewer = this.viewer;
    const current = viewer.current;
    const ctx = this.canvas.getContext('2d');
    const W = this.canvas.width;
    const H = this.canvas.height;
    ctx.clearRect(0, 0, W, H);
    if (!current) return;

    const plan = this.planFor(current.floor);
    if (!plan) { this.layout = null; return; }

    const worldW = plan.maxY - plan.minY;
    const worldH = plan.maxX - plan.minX;
    const scale = Math.min(W / worldW, H / worldH);
    this.layout = { plan, scale, ox: (W - worldW * scale) / 2, oy: (H - worldH * scale) / 2 };

    const img = this.image(plan);
    if (img.complete && img.naturalWidth) {
      ctx.drawImage(img, this.layout.ox, this.layout.oy, worldW * scale, worldH * scale);
    }

    const r = (this.compact ? 4 : 7) * this.ratio;

    ctx.strokeStyle = 'rgba(255,255,255,0.25)';
    ctx.lineWidth = 1.5 * this.ratio;
    for (const p of viewer.points.values()) {
      if (p.floor !== current.floor) continue;
      for (const id of p.links) {
        const q = viewer.points.get(id);
        if (q.floor !== p.floor || q.id < p.id) continue;
        const [x1, y1] = this.project(p.position.x, p.position.y);
        const [x2, y2] = this.project(q.position.x, q.position.y);
        ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
      }
    }

    // View cone for the current point.
    const [cx, cy] = this.project(current.position.x, current.position.y);
    const heading = -viewer.yaw;
    const halfFov = viewer.horizontalFov() / 2;
    const reach = (this.compact ? 34 : 70) * this.ratio;
    const angle = (a) => [cx + Math.sin(a) * reach, cy - Math.cos(a) * reach];
    ctx.fillStyle = 'rgba(63,169,245,0.28)';
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    for (let i = 0; i <= 16; i++) ctx.lineTo(...angle(heading - halfFov + (2 * halfFov * i) / 16));
    ctx.closePath();
    ctx.fill();

    for (const p of viewer.points.values()) {
      if (p.floor !== current.floor) continue;
      const [x, y] = this.project(p.position.x, p.position.y);
      ctx.beginPath();
      ctx.arc(x, y, p === current ? r * 1.3 : r, 0, Math.PI * 2);
      ctx.fillStyle = p === current ? '#3fa9f5' : 'rgba(255,255,255,0.9)';
      ctx.fill();
      ctx.lineWidth = 2 * this.ratio;
      ctx.strokeStyle = 'rgba(0,0,0,0.6)';
      ctx.stroke();
    }
  }

  click(e) {
    if (!this.layout) return;
    const rect = this.canvas.getBoundingClientRect();
    const mx = (e.clientX - rect.left) * this.ratio;
    const my = (e.clientY - rect.top) * this.ratio;
    let best = null;
    let bestDistance = 18 * this.ratio;
    for (const p of this.viewer.points.values()) {
      if (p.floor !== this.viewer.current.floor) continue;
      const [x, y] = this.project(p.position.x, p.position.y);
      const d = Math.hypot(x - mx, y - my);
      if (d < bestDistance) { best = p; bestDistance = d; }
    }
    if (!best) return;
    if (!this.compact) {
      this.viewer.mode = 'pano';
      this.viewer.updateModeUi();
      this.viewer.show(best, false);
    } else {
      this.viewer.show(best, true);
    }
  }
}

// ---------------------------------------------------------------------------------------------

function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen?.();
}

function showMessage(html) {
  $('message').innerHTML = html;
  $('message').hidden = false;
}

async function main() {
  if (location.protocol === 'file:') {
    showMessage('Browsers block panoramas opened straight from disk.<br>Use <b>Open Viewer</b> on the Pano 360 Camera in Unreal, or serve this folder with any web server.');
    return;
  }

  const baseUrl = new URL('./', location.href);
  let tour;
  try {
    const response = await fetch(new URL('tour.json', baseUrl), { cache: 'no-cache' });
    tour = await response.json();
  } catch (error) {
    showMessage('Could not load tour.json from this folder.');
    console.error(error);
    return;
  }
  if (!tour.points?.length) {
    showMessage('This tour has no panoramas yet.');
    return;
  }

  // A hand-edited or truncated link (#%E0) must not stop the tour from starting; it just opens at the first point.
  let startId = '';
  try {
    startId = decodeURIComponent(location.hash.slice(1));
  } catch {
    console.warn('Ignoring malformed point id in the URL:', location.hash);
  }

  const viewer = new TourViewer(tour, baseUrl);
  window.tourViewer = viewer;
  await viewer.start(startId);
}

main();
