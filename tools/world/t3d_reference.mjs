#!/usr/bin/env node
// SPDX-License-Identifier: MIT
//
// t3d_reference.mjs -- dump the numbers T3D (spjinx/t3d, the user's browser map
// viewer) computes for one GW2 map, as a JSON reference that castlemist's own
// map reader is checked against.
//
// T3D is GPL-3 and castlemist is MIT, so nothing of T3D is copied here. The
// script loads T3D's *parser* package at run time from a checkout outside this
// repo (`--t3d`) and re-derives the placement arithmetic of T3D's three.js
// renderers from their documented behaviour; every rule cites the T3D file and
// line it reproduces (T3D commit recorded in the output as `t3dCommit`).
//
// Usage:
//   node tools/world/t3d_reference.mjs --t3d <t3d checkout> \
//        --map-bytes <decompressed map packfile> --file-id <id> --out <json>
//
// The parser must be built first: `npm ci && npm run build` in <t3d>/parser.
// See tools/world/README.md for every field's convention.

import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { pathToFileURL } from "node:url";

// ---------------------------------------------------------------- arguments

function parseArgs(argv) {
    const out = {};
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (!a.startsWith("--")) throw new Error(`unexpected argument: ${a}`);
        const v = argv[i + 1];
        if (v === undefined || v.startsWith("--")) throw new Error(`missing value for ${a}`);
        out[a.slice(2)] = v;
        i++;
    }
    for (const k of ["t3d", "map-bytes", "file-id", "out"]) {
        if (!out[k]) throw new Error(`missing --${k}`);
    }
    return out;
}

// ---------------------------------------------------------------- small maths
// Plain 4x4 matrices, column-major (element [col*4 + row]) like three.js
// Matrix4.elements and glTF.

const r6 = (v) => {
    const r = Math.round(v * 1e6) / 1e6;
    return Object.is(r, -0) ? 0 : r;
};

function mat4Mul(a, b) {
    const o = new Array(16).fill(0);
    for (let c = 0; c < 4; c++)
        for (let r = 0; r < 4; r++) {
            let s = 0;
            for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = s;
        }
    return o;
}

// Build a column-major 4x4 from a row-major 3x3 linear part and a translation.
function mat4From3(m3, t = [0, 0, 0]) {
    return [
        m3[0], m3[3], m3[6], 0,
        m3[1], m3[4], m3[7], 0,
        m3[2], m3[5], m3[8], 0,
        t[0], t[1], t[2], 1,
    ];
}

function mul3(a, b) {
    const o = new Array(9).fill(0);
    for (let r = 0; r < 3; r++)
        for (let c = 0; c < 3; c++)
            for (let k = 0; k < 3; k++) o[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
    return o;
}

const rotX = (t) => [1, 0, 0, 0, Math.cos(t), -Math.sin(t), 0, Math.sin(t), Math.cos(t)];
const rotY = (t) => [Math.cos(t), 0, Math.sin(t), 0, 1, 0, -Math.sin(t), 0, Math.cos(t)];
const rotZ = (t) => [Math.cos(t), -Math.sin(t), 0, Math.sin(t), Math.cos(t), 0, 0, 0, 1];

// three.js Euler(x, y, z, "ZXY") as a rotation matrix is Rz(z) * Rx(x) * Ry(y)
// (three.js Matrix4.makeRotationFromEuler / Quaternion.setFromEuler, order ZXY).
const eulerZXY = (x, y, z) => mul3(mul3(rotZ(z), rotX(x)), rotY(y));

// T3D's axis map from GW2 map space into three.js: (x, y, z) -> (x, -z, -y)
// (PropertiesRenderer.ts:202, HavokRenderer.ts:249, and the model vertex
// reader RenderUtils.ts:649-656, which reads file (a, b, c) and writes
// (a, -c, -b)). It is its own inverse, so map space = A * three.
const A3 = [1, 0, 0, 0, 0, -1, 0, -1, 0];
const A4 = mat4From3(A3);

// T3D's mapping of Havok hull vertices into three.js: (v0, v1, v2) ->
// (v0, v2, -v1) (HavokRenderer.ts:288-292). A different map from A3.
const HAVOK_LOCAL4 = mat4From3([1, 0, 0, 0, 0, 1, 0, -1, 0]);

// T3D's object matrix: translation (x, -z, -y), Euler (r0, -r2, -r1) "ZXY",
// uniform scale s, composed as T * R * S
// (PropertiesRenderer.ts:196-205: makeRotationFromEuler, scale, setPosition;
//  HavokRenderer.ts:244-261: compose(position, quaternion(euler), scale)).
function t3dObjectMatrix(pos, rot, scale) {
    const R = rot ? eulerZXY(rot[0], -rot[2], -rot[1]) : [1, 0, 0, 0, 1, 0, 0, 0, 1];
    const RS = R.map((v) => v * scale);
    const t = pos ? [pos[0], -pos[2], -pos[1]] : [0, 0, 0];
    return mat4From3(RS, t);
}

// World matrix in map space = A^-1 * M_three * L, where L is how T3D maps the
// file's local vertices into three.js (A for model meshes, HAVOK_LOCAL4 for
// hulls). It takes a vertex *as stored in its file* to GW2 map space exactly
// where T3D draws it.
const toMapSpace = (mThree, local4) => mat4Mul(mat4Mul(A4, mThree), local4).map(r6);

// ---------------------------------------------------------------- terrain

// Segments per chunk side and stored samples per side. T3D uses the trn's
// verticesPerChunkSide when the stored sample count is (side+3)^2 per chunk,
// and otherwise its legacy 32 segments / 35 samples
// (after spjinx/t3d TerrainRenderer.ts:15-36, getChunkResolution).
function chunkResolution(trn, warnings) {
    const LEGACY = 32;
    const side = trn.verticesPerChunkSide ?? LEGACY;
    const count = trn.chunkArray.length;
    if (trn.verticesPerChunkSide === undefined) {
        warnings.push("trn has no verticesPerChunkSide; T3D uses its legacy 32 segments / 35 samples (TerrainRenderer.ts:15-19)");
    }
    const fits = count > 0 && trn.heightMapArray.length === (side + 3) * (side + 3) * count;
    if (!fits) {
        const perChunk = count > 0 ? trn.heightMapArray.length / count : 0;
        warnings.push(`trn samples per chunk ${perChunk} != (${side}+3)^2; T3D falls back to 32/35 (TerrainRenderer.ts:32-35)`);
    }
    const segments = fits ? side : LEGACY;
    return { segments, sampleWidth: segments + 3 };
}

// Reproduces TerrainRenderer.loadPagedImageCallback's chunk placement and the
// height grid it hands to createTerrainHeightSampler. All values here are in
// three.js space (X = map x, Z = -map y), as T3D holds them.
function buildTerrain(trn, parm, warnings) {
    const { segments, sampleWidth } = chunkResolution(trn, warnings);

    // TerrainRenderer.ts:192-195 (parseNumChunks).
    const xChunks = Math.sqrt((trn.dims[0] * trn.chunkArray.length) / trn.dims[1]);
    const yChunks = trn.chunkArray.length / xChunks;
    if (!Number.isInteger(xChunks) || !Number.isInteger(yChunks)) {
        warnings.push(`T3D chunk grid is not integral: ${xChunks} x ${yChunks}`);
    }

    // TerrainRenderer.ts:249-254: chunk size = rect extent / chunk count.
    const rect = parm.rect;
    const cdx = (rect[2] - rect[0]) / xChunks;
    const cdy = (rect[3] - rect[1]) / yChunks;

    const chunks = [];
    let n = 0;
    let bounds = null;
    for (let cy = 0; cy < yChunks; cy++) {
        for (let cx = 0; cx < xChunks; cx++) {
            const chunkIndex = cy * xChunks + cx; // TerrainRenderer.ts:338

            // TerrainRenderer.ts:443-462: walk the stored sampleWidth^2 samples,
            // keep the inner (segments+1)^2 (drop the outer ring), row-major,
            // and store them negated as the three.js Y of each vertex.
            const heights = new Float32Array((segments + 1) * (segments + 1));
            let cn = 0;
            for (let y = 0; y < sampleWidth; y++) {
                for (let x = 0; x < sampleWidth; x++) {
                    if (x !== 0 && x !== sampleWidth - 1 && y !== 0 && y !== sampleWidth - 1) {
                        heights[cn++] = -trn.heightMapArray[n];
                    }
                    n++;
                }
            }

            // TerrainRenderer.ts:484-502: chunk centre. Y offset depends on
            // whether numChunksD_2 is even (rect[1] + cdy/2) or odd
            // (rect[1] - cdy/2); T3D does not explain this.
            const posX = rect[0] + (cx + 0.5) * cdx;
            const posZ = rect[1] + (cy + (yChunks % 2 === 0 ? 0.5 : -0.5)) * cdy;

            // TerrainRenderer.ts:504-520 (mapRect).
            const x1 = posX - cdx / 2, x2 = posX + cdx / 2, z1 = posZ - cdy / 2, z2 = posZ + cdy / 2;
            if (!bounds) bounds = { x1, x2, z1, z2 };
            bounds.x1 = Math.min(bounds.x1, x1);
            bounds.x2 = Math.max(bounds.x2, x2);
            bounds.z1 = Math.min(bounds.z1, z1);
            bounds.z2 = Math.max(bounds.z2, z2);

            // TerrainRenderer.ts:525-535. The PlaneGeometry is flipped in Y
            // (465-467) then rotated +90deg about X (482), which sends plane
            // row 0 to the chunk's minimum three.js Z and the stored height h
            // to three.js Y = -h; the sampler (85-117) reads the same layout.
            chunks[chunkIndex] = {
                minX: posX - cdx / 2, maxX: posX + cdx / 2,
                minZ: posZ - cdy / 2, maxZ: posZ + cdy / 2,
                columns: segments + 1, rows: segments + 1,
                heights,
            };
        }
    }

    // TerrainRenderer.ts:536-541, 560-569: sampler origin = smallest chunk min.
    let originX = null, originZ = null;
    for (const c of chunks) {
        if (originX === null || c.minX < originX) originX = c.minX;
        if (originZ === null || c.minZ < originZ) originZ = c.minZ;
    }

    return { segments, sampleWidth, xChunks, yChunks, cdx, cdy, chunks, bounds, originX, originZ };
}

// Height at three.js (x, z), or null off the terrain: pick the chunk cell
// holding the point (counted from the sampler origin, clamped to the last
// chunk), then interpolate bilinearly in that chunk's inner height grid,
// whose corners sit on the chunk's min/max X and Z
// (after spjinx/t3d TerrainRenderer.ts:85-148, the terrain height sampler).
const lerp = (a, b, t) => a + (b - a) * t;

function sampleTerrain(t, x, z) {
    const lx = x - t.originX, lz = z - t.originZ;
    if (lx < 0 || lz < 0) return null;
    const chunk = t.chunks[Math.min(t.yChunks - 1, Math.floor(lz / t.cdy)) * t.xChunks +
                           Math.min(t.xChunks - 1, Math.floor(lx / t.cdx))];
    if (!chunk || chunk.maxX <= chunk.minX || chunk.maxZ <= chunk.minZ) return null;

    // Fraction across the chunk; outside [0, 1] means the point is not on it.
    const u = (x - chunk.minX) / (chunk.maxX - chunk.minX);
    const v = (z - chunk.minZ) / (chunk.maxZ - chunk.minZ);
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) return null;

    // Grid coordinates; u, v in [0, 1] keep them inside the grid.
    const last = chunk.columns - 1;
    const gx = u * last, gz = v * (chunk.rows - 1);
    const i = Math.floor(gx), j = Math.floor(gz);
    const i1 = Math.min(last, i + 1), j1 = Math.min(chunk.rows - 1, j + 1);
    const at = (col, row) => chunk.heights[row * chunk.columns + col];
    return lerp(lerp(at(i, j), at(i1, j), gx - i), lerp(at(i, j1), at(i1, j1), gx - i), gz - j);
}

// Map-space height at map (x, y): T3D samples three.js (X = x, Z = -y) and
// holds three.js Y = -h, so map z = -Y (inverse of the (x,-z,-y) map).
function mapHeightAt(t, x, y) {
    const Y = sampleTerrain(t, x, -y);
    return Y === null ? null : -Y;
}

// ---------------------------------------------------------------- output

// One top-level key per line; arrays of rows (and the collision sample) one
// row per line, so diffs of the reference stay readable.
function formatJson(obj) {
    const rowsOf = (v) => (Array.isArray(v) && v.length && typeof v[0] === "object" && v[0] !== null);
    const block = (v, ind) => {
        if (rowsOf(v)) return "[\n" + v.map((r) => ind + "  " + JSON.stringify(r)).join(",\n") + "\n" + ind + "]";
        if (v && typeof v === "object" && !Array.isArray(v) && Object.values(v).some(rowsOf)) {
            const keys = Object.keys(v);
            return "{\n" + keys.map((k) => `${ind}  ${JSON.stringify(k)}: ${block(v[k], ind + "  ")}`).join(",\n") + "\n" + ind + "}";
        }
        return JSON.stringify(v);
    };
    return block(obj, "") + "\n";
}

// ---------------------------------------------------------------- main

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const t3d = path.resolve(args.t3d);
    const fileId = Number(args["file-id"]);
    const warnings = [];

    const parserPath = path.join(t3d, "parser", "build", "t3d-parser.mjs");
    if (!fs.existsSync(parserPath)) {
        throw new Error(`${parserPath} not found; run 'npm ci && npm run build' in ${path.join(t3d, "parser")}`);
    }
    const T3DParser = await import(pathToFileURL(parserPath).href);

    let t3dCommit = "unknown";
    try {
        t3dCommit = execFileSync("git", ["-C", t3d, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    } catch {
        warnings.push("could not read the T3D checkout's git commit");
    }

    // Parse the packfile the way T3D's library does: one FileParser over the
    // whole decompressed map (the renderers' settings.mapFile). The parser
    // logs every chunk on console.log and parse errors on console.error;
    // silence the first and keep the second as warnings.
    const buf = fs.readFileSync(args["map-bytes"]);
    const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    const { log, error } = console;
    console.log = () => {};
    console.error = (...a) => warnings.push(`t3d parser: ${a.join(" ")}`);
    let file;
    try {
        file = new T3DParser.FileParser(ab);
    } finally {
        console.log = log;
        console.error = error;
    }
    if (file.header.type !== "mapc") throw new Error(`file ${fileId} is a '${file.header.type}' packfile, not a map ('mapc')`);

    const chunkOf = (name) => file.getChunk(name);
    const parm = chunkOf("parm")?.data;
    const trn = chunkOf("trn")?.data;
    if (!parm || !trn) throw new Error(`file ${fileId} lacks a parm or trn chunk`);

    // ---- terrain
    const terrain = buildTerrain(trn, parm, warnings);
    const rect = Array.from(parm.rect);

    // Terrain bounds back in map space: map y = -three.js Z.
    const b = terrain.bounds;
    const terrainBounds = [b.x1, -b.z2, b.x2, -b.z1].map(r6);
    if (terrainBounds.some((v, i) => Math.abs(v - rect[i]) > 1e-3)) {
        warnings.push(`T3D terrain bounds in map space ${JSON.stringify(terrainBounds)} differ from parm.rect ${JSON.stringify(rect)}`);
    }

    // Height sample: 16x16 cell centres of parm.rect, row by row from rect y0
    // (outer loop j) and x0 (inner loop i).
    const N = 16;
    const heights = [];
    let missing = 0;
    for (let j = 0; j < N; j++) {
        for (let i = 0; i < N; i++) {
            const x = rect[0] + ((i + 0.5) * (rect[2] - rect[0])) / N;
            const y = rect[1] + ((j + 0.5) * (rect[3] - rect[1])) / N;
            const z = mapHeightAt(terrain, x, y);
            if (z === null || !Number.isFinite(z)) missing++;
            heights.push([r6(x), r6(y), z === null ? null : r6(z)]);
        }
    }
    if (missing) warnings.push(`${missing} of ${N * N} height positions fall outside T3D's terrain (z = null)`);

    // ---- terrain materials, first 16 chunks (TerrainRenderer.ts:337-381).
    const allMaterials = trn.materials.materials;
    const allTextures = trn.materials.texFileArray;
    const terrainMaterials = [];
    for (let chunk = 0; chunk < Math.min(16, trn.chunkArray.length); chunk++) {
        const cx = chunk % terrain.xChunks;
        const cy = Math.floor(chunk / terrain.xChunks);
        const idx = allMaterials[chunk].loResMaterial.texIndexArray; // :344
        const textures = [];
        for (let gi = 0; gi < idx.length / 2; gi++) textures.push(allTextures[idx[gi]].filename); // :362-363
        terrainMaterials.push({ chunk, textures, pickerPage: [Math.floor(cx / 4), Math.floor(cy / 4)] }); // :340-341
    }

    // ---- props (PropertiesRenderer.ts:56-67, 164-171, 196-205).
    const PROP_GROUPS = ["propArray", "propAnimArray", "propInstanceArray", "propMetaArray"]; // :63-67
    const prp2 = chunkOf("prp2")?.data;
    const props = [];
    const propCounts = {};
    if (!prp2) warnings.push("no prp2 chunk; T3D renders no props");
    for (const group of PROP_GROUPS) {
        const arr = prp2?.[group] ?? [];
        let count = 0, written = 0;
        for (const prop of arr) {
            // A prop, then each entry of its transforms[] (:164-171); a
            // transform inherits the prop's model (filename).
            const rows = [prop, ...(prop.transforms ?? [])];
            rows.forEach((p, k) => {
                count++;
                if (written >= 50) return;
                written++;
                const row = {
                    fileId: prop.filename,
                    group,
                    pos: Array.from(p.position),
                    rot: Array.from(p.rotation),
                    scale: p.scale,
                    world: toMapSpace(t3dObjectMatrix(p.position, p.rotation, p.scale), A4),
                };
                if (k > 0) row.transform = k - 1;
                props.push(row);
            });
        }
        propCounts[group] = count;
    }

    // ---- collision (HavokRenderer.ts:349-390, 108-204, 244-261).
    const havk = chunkOf("havk")?.data;
    // `sample` = first 20 rows overall (obs, prop, zone order); obs models
    // carry no rotation, so `sampleByGroup` (first 20 rows of each group)
    // is added to exercise rotation and scale too.
    const collision = {
        instances: 0,
        instancesByGroup: { obs: 0, prop: 0, zone: 0 },
        sample: [],
        sampleByGroup: { obs: [], prop: [], zone: [] },
    };
    if (!havk) {
        warnings.push("no havk chunk; T3D renders no collision");
    } else {
        // Order used here: obs, prop, zone (castlemist's placement order).
        // T3D itself renders prop, zone, obs (HavokRenderer.ts:382-389) and
        // then buckets by collision index, so T3D has no global row order.
        const groups = [
            ["obs", havk.obsModels ?? []],
            ["prop", havk.propModels ?? []],
            ["zone", havk.zoneModels ?? []],
        ];
        let badGeom = 0;
        for (const [group, models] of groups) {
            models.forEach((model, index) => {
                // obs models get scale 1 (HavokRenderer.ts:373-375).
                const scaleField = group === "obs" ? 1 : model.scale;
                // animations[geometries[g].animations[last]] (:189-203).
                const geom = havk.geometries[model.geometryIndex];
                if (!geom) { badGeom++; return; }
                const anim = havk.animations[geom.animations[geom.animations.length - 1]];
                if (!anim || !anim.collisionIndices) return; // :137-139
                for (const collisionIndex of anim.collisionIndices) {
                    if (!havk.collisions[collisionIndex]) continue; // :143-146
                    collision.instances++;
                    collision.instancesByGroup[group]++;
                    const byGroup = collision.sampleByGroup[group];
                    if (collision.sample.length >= 20 && byGroup.length >= 20) continue;
                    // scale 32 * model.scale; falsy -> 1 (:247, 250-253).
                    let s = 32 * scaleField;
                    if (!s) s = 1;
                    const row = {
                        group,
                        index,
                        geometryIndex: model.geometryIndex,
                        collisionIndex,
                        world: toMapSpace(t3dObjectMatrix(model.translate, model.rotate, s), HAVOK_LOCAL4),
                    };
                    if (collision.sample.length < 20) collision.sample.push(row);
                    if (byGroup.length < 20) byGroup.push(row);
                }
            });
        }
        if (badGeom) {
            warnings.push(`${badGeom} havk placements have a geometryIndex out of range (T3D would throw at HavokRenderer.ts:195); skipped`);
        }
    }

    // ---- water. T3D's renderers do not read `watr`: they draw one flat
    // plane at three.js Y = 0 over the terrain bounds (TerrainRenderer.ts:
    // 180-190, 556). The numbers below are the parser's `watr` values as stored.
    const watrChunk = chunkOf("watr");
    const water = { surfaces: 0, z: [] };
    if (!watrChunk) {
        warnings.push("no watr chunk");
    } else if (!Array.isArray(watrChunk.data?.waterSurfaces)) {
        warnings.push(`watr v${watrChunk.header.chunkVersion} has no waterSurfaces field`);
    } else {
        water.surfaces = watrChunk.data.waterSurfaces.length;
        water.z = watrChunk.data.waterSurfaces.map((s) => s.waterSurfaceZ);
        water.planeZ = watrChunk.data.waterPlaneZ;
    }
    if (havk && havk.waterSurfaceZ !== undefined) water.havkWaterSurfaceZ = havk.waterSurfaceZ;

    const out = {
        map: fileId,
        t3dCommit,
        rect,
        terrainBounds,
        chunks: [terrain.xChunks, terrain.yChunks],
        segments: terrain.segments,
        samplesPerChunkStored: terrain.sampleWidth,
        heights,
        props,
        propCounts,
        collision,
        water,
        terrainMaterials,
        warnings,
    };

    // Diagnostic only (stderr): how far each propArray prop's stored z is from
    // T3D's terrain height under its (x, y).
    if (prp2) {
        const d = [];
        for (const p of prp2.propArray) {
            const h = mapHeightAt(terrain, p.position[0], p.position[1]);
            if (h !== null) d.push(Math.abs(p.position[2] - h));
        }
        d.sort((a, c) => a - c);
        const q = (f) => (d.length ? d[Math.floor(f * (d.length - 1))].toFixed(1) : "n/a");
        console.error(`[${fileId}] |prop z - T3D terrain z| over ${d.length} propArray props: p25 ${q(0.25)} median ${q(0.5)} p75 ${q(0.75)}`);
    }

    fs.mkdirSync(path.dirname(path.resolve(args.out)), { recursive: true });
    fs.writeFileSync(args.out, formatJson(out));
    console.log(`wrote ${args.out}: chunks ${out.chunks.join("x")}, ${props.length} props, ` +
        `${collision.instances} collision instances, ${water.surfaces} water surfaces, ${warnings.length} warnings`);
}

main().catch((e) => {
    console.error(e.message ?? e);
    process.exit(1);
});
