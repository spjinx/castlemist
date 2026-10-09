// havk_sequences.mjs -- the census behind docs/research/gw2-world-frame.md §7.1:
// what a havk geometry's `animations[]` entries are, and which one a
// placement's own `sequence` names.
//
// Usage (map bytes extracted as in tools/world/README.md):
//   node tools/world/havk_sequences.mjs --t3d <t3d checkout> --map-bytes <file>
//
// Reads the packfile with T3D's parser package, loaded at run time from the
// checkout outside this repo (as t3d_reference.mjs does); nothing of T3D is
// copied. Prints numbers only.

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

function arg(name) {
    const i = process.argv.indexOf(`--${name}`);
    if (i < 0 || i + 1 >= process.argv.length) throw new Error(`missing --${name}`);
    return process.argv[i + 1];
}

const T3DParser = await import(pathToFileURL(path.join(path.resolve(arg("t3d")), "parser", "build", "t3d-parser.mjs")).href);
const buf = fs.readFileSync(arg("map-bytes"));
const { log } = console;
console.log = () => {};
let file;
try {
    file = new T3DParser.FileParser(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
} finally {
    console.log = log;
}
const havk = file.getChunk("havk")?.data;
const prp2 = file.getChunk("prp2")?.data;
if (!havk) throw new Error("no havk chunk");
const list = (o) => Array.from(Object.values(o ?? []));
const animsOf = (g) => list(havk.geometries[g]?.animations).map((i) => havk.animations[i]);
const count = (obj, key) => { obj[key] = (obj[key] || 0) + 1; };

// 1. Animations per geometry, and the sequence tokens animations carry.
const perGeometry = {};
for (const g of havk.geometries) count(perGeometry, list(g.animations).length);
const sequences = {};
for (const a of havk.animations) count(sequences, String(a.sequence));
const topSequences = Object.entries(sequences).sort((a, b) => b[1] - a[1]).slice(0, 4);
console.log(`geometries ${havk.geometries.length}, animations ${havk.animations.length}, collisions ${havk.collisions.length}`);
console.log("animations per geometry:", JSON.stringify(perGeometry));
console.log(`distinct animation sequences ${Object.keys(sequences).length}; most common:`, JSON.stringify(topSequences));

// 2. Within one geometry, are the sequences distinct?
let multi = 0, distinct = 0;
for (let g = 0; g < havk.geometries.length; ++g) {
    const an = animsOf(g);
    if (an.length < 2) continue;
    ++multi;
    if (new Set(an.map((a) => String(a.sequence))).size === an.length) ++distinct;
}
console.log(`geometries with 2+ animations: ${multi}; all sequences distinct within the geometry: ${distinct}; ` +
    `with none: ${havk.geometries.filter((g) => list(g.animations).length === 0).length}`);
const topSequence = topSequences.length ? topSequences[0][0] : "";
console.log(`propModels whose sequence is the most common animation sequence (${topSequence}): ` +
    `${havk.propModels.filter((m) => String(m.sequence) === topSequence).length} of ${havk.propModels.length}; ` +
    `prop or zone placements with scale 0: ${[...havk.propModels, ...havk.zoneModels].filter((m) => !m.scale).length}`);

// 3. propModels: does the placement's `sequence` name one of its geometry's
//    animations, and is that one animations[last]?
const where = { first: 0, middle: 0, last: 0, none: 0 };
let rowsLast = 0, rowsBySequence = 0;
for (const m of havk.propModels) {
    const an = animsOf(m.geometryIndex);
    if (!an.length) continue;
    const k = an.findIndex((a) => a.sequence === m.sequence);
    const last = an[an.length - 1];
    rowsLast += list(last.collisionIndices).length;
    rowsBySequence += list((k >= 0 ? an[k] : last).collisionIndices).length;
    if (an.length < 2) continue;
    count(where, k < 0 ? "none" : k === an.length - 1 ? "last" : k === 0 ? "first" : "middle");
}
console.log(`propModels ${havk.propModels.length}; on geometries with 2+ animations, the placement's sequence is animation:`,
    JSON.stringify(where));
console.log(`prop collision rows: animations[last] ${rowsLast}, animation named by the placement's sequence ${rowsBySequence}`);

// 4. propModels.token against prp2 guid; the linked prop's animSequence.
if (prp2) {
    const byGuid = new Map();
    for (const group of ["propArray", "propAnimArray", "propInstanceArray", "propMetaArray"])
        for (const p of prp2[group] ?? []) byGuid.set(p.guid, p);
    let linked = 0, samePlace = 0, animProps = 0, sameSequence = 0;
    for (const m of havk.propModels) {
        const p = byGuid.get(m.token);
        if (!p) continue;
        ++linked;
        const pos = list(p.position), t = list(m.translate);
        if (Math.abs(p.scale - m.scale) < 1e-4 && pos.every((v, k) => Math.abs(v - t[k]) < 0.01)) ++samePlace;
        if (p.animSequence !== undefined) { ++animProps; if (p.animSequence === m.sequence) ++sameSequence; }
    }
    console.log(`propModels whose token is a prp2 base prop's guid: ${linked} (same translate and scale: ${samePlace}); ` +
        `of those from propAnimArray ${animProps}, animSequence == sequence: ${sameSequence}`);
}

// 5. zoneModels and obsModels carry no sequence: their geometries' patterns.
for (const [name, models] of [["zoneModels", havk.zoneModels], ["obsModels", havk.obsModels]]) {
    const patterns = {};
    for (const m of models ?? [])
        count(patterns, animsOf(m.geometryIndex).map((a) => `${a.sequence}:${list(a.collisionIndices).length}`).join(" "));
    console.log(`${name} ${(models ?? []).length}; geometry patterns (sequence:collisions):`,
        JSON.stringify(Object.entries(patterns).sort((a, b) => b[1] - a[1]).slice(0, 3)));
}
