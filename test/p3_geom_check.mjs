/* =====================================================================
 * creo-hub M3d d1: demo3.boom prism geometry STATIC gate (Q-M3d-4).
 *
 * Why: ISSUE-L2 (docs\25 §7) - one row of P3_FACES had three transcription
 * deviations from the PTC sample (v3 / origin / contour), Creo dropped the
 * face, the open shell degraded attempt_make_solid -> quilt instead of
 * solid. Fixed in M3d d1 by restoring the row verbatim (docs\29 §2).
 * This gate mechanically guards the whole bug CLASS offline: it parses the
 * real C tables out of src\demo3\demo3_adapter.c and asserts geometry
 * invariants that any valid closed manifold prism must satisfy.
 *
 * Checks (per face F-*, global G-*):
 *   F-C1  v3 == v1 x v2               (right-hand triple, all-sample rule)
 *   F-C1b v1,v2,v3 unit + v1 _|- v2   (orthonormal plane frame)
 *   F-C3  origin AND every contour-edge endpoint on plane(v3, origin)
 *   F-C5  face id appears in edge.s[] for every contour edge
 *   F-C6  orient * dot(v3, P - origin) <= 0 for ALL solid vertices
 *         (material half-space convention, empirically pinned by the PTC
 *          sample UgImportfeatCreate.c: holds for all 6 reference faces)
 *   F-G8  contour closes as ONE directed 4-cycle using each edge once;
 *         edge direction = dir flag on THIS face (NO_FLIP(0)=p1->p2,
 *         FLIP(1)=p2->p1). Convention verified against all 6 sample faces
 *         (PTK headers under-document dir<->traversal coupling; the sample
 *          is the reference implementation - docs\29 §2.2).
 *   G-G1  table shape: 12 edges id 1..12 unique, 6 faces
 *   G-G2  every edge referenced by exactly 2 face contours (manifold)
 *   G-G3  every edge referenced at least once (implied by G2, explicit)
 *   G-G4  Euler V-E+F == 2 (V = unique endpoints)
 *   G-G5  edge.s[] set == set of faces referencing the edge
 *   G-G7  edge dirs on its two faces are opposite (orientability)
 *   G-G9  bounding box == 12 x 10 x 8 (dimension typo catcher; matches
 *         plugin.json descForLLM "12x10x8 prism")
 *
 * Usage:
 *   node test\p3_geom_check.mjs [--selftest] [path\to\demo3_adapter.c]
 *   --selftest: run the checker against mutated tables (historical
 *     ISSUE-L2 buggy row + orient flip + dir-pair flip); PASS iff every
 *     mutation is CAUGHT by its expected checks (gate detection proof).
 * Exit: 0 = PASS, 1 = FAIL (or uncaught mutation), 2 = parse/usage error.
 * ===================================================================== */
import { readFileSync } from "node:fs";
import { join } from "node:path";

const EPS = 1e-9;

/* ------------------------------------------------------------- parsing */

function parseNums(s) {
  return s.split(",").map((t) => {
    const n = Number(t.trim());
    if (!Number.isFinite(n)) throw new Error(`bad number '${t}' in table row`);
    return n;
  });
}

function parseTables(src) {
  const edges = [];
  const faces = [];
  const eBlk = /P3_EDGES\[12\]\s*=\s*\{([\s\S]*?)\};/m.exec(src);
  const fBlk = /P3_FACES\[6\]\s*=\s*\{([\s\S]*?)\};/m.exec(src);
  if (!eBlk || !fBlk) throw new Error("P3_EDGES/P3_FACES table block not found");
  const eRow = /\{\s*(-?\d+)\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*\}/g;
  let m;
  while ((m = eRow.exec(eBlk[1])) !== null) {
    const p1 = parseNums(m[4]);
    const p2 = parseNums(m[5]);
    if (p1.length !== 3 || p2.length !== 3) throw new Error(`edge ${m[1]}: points must have 3 coords`);
    edges.push({ id: Number(m[1]), s: parseNums(m[2]), dir: parseNums(m[3]), p1, p2 });
  }
  const fRow = /\{\s*(-?\d+)\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*\}/g;
  while ((m = fRow.exec(fBlk[1])) !== null) {
    const v1 = parseNums(m[2]), v2 = parseNums(m[3]), v3 = parseNums(m[4]), o = parseNums(m[5]);
    if (v1.length !== 3 || v2.length !== 3 || v3.length !== 3 || o.length !== 3)
      throw new Error(`face row ${faces.length + 1}: vectors must have 3 coords`);
    faces.push({ orient: Number(m[1]), v1, v2, v3, o, edges: parseNums(m[6]).map(Number) });
  }
  return { edges, faces };
}

/* ------------------------------------------------------------- vector  */

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
const len = (a) => Math.hypot(a[0], a[1], a[2]);
const key = (p) => p.map((c) => (Object.is(c, -0) ? 0 : c)).join("|");

/* ------------------------------------------------------------- checks  */

function runChecks(t) {
  const fails = [];
  const fail = (id, msg) => fails.push(`${id}: ${msg}`);
  const { edges, faces } = t;
  const edgeById = new Map(edges.map((e) => [e.id, e]));

  /* G-G1 table shape */
  if (edges.length !== 12) fail("G-G1", `expected 12 edge rows, parsed ${edges.length}`);
  if (faces.length !== 6) fail("G-G1", `expected 6 face rows, parsed ${faces.length}`);
  const ids = new Set(edges.map((e) => e.id));
  if (ids.size !== edges.length) fail("G-G1", "duplicate edge ids");
  for (let i = 1; i <= 12; i++) if (!ids.has(i)) fail("G-G1", `edge id ${i} missing`);

  /* vertices + bbox */
  const verts = new Map();
  for (const e of edges) {
    verts.set(key(e.p1), e.p1);
    verts.set(key(e.p2), e.p2);
  }
  const pts = [...verts.values()];
  /* G-G4 Euler */
  if (pts.length - edges.length + faces.length !== 2)
    fail("G-G4", `Euler V-E+F = ${pts.length}-${edges.length}+${faces.length} != 2`);
  /* G-G9 bbox 12x10x8 */
  if (pts.length === 8) {
    const mn = [0, 1, 2].map((i) => Math.min(...pts.map((p) => p[i])));
    const mx = [0, 1, 2].map((i) => Math.max(...pts.map((p) => p[i])));
    const dim = mx.map((v, i) => v - mn[i]);
    if (dim.some((d, i) => Math.abs(d - [12, 10, 8][i]) > EPS) || mn.some((v) => Math.abs(v) > EPS))
      fail("G-G9", `bbox min=${mn} dims=${dim}, expected min 0,0,0 dims 12,10,8`);
  } else {
    fail("G-G9", `expected 8 unique vertices, got ${pts.length}`);
  }

  /* face referencing map (for G-G2/3/5) */
  const faceRefs = new Map(edges.map((e) => [e.id, []]));
  faces.forEach((f, fi) => {
    const surfId = fi + 1;
    if (f.edges.length !== 4) fail("F-C5", `face ${surfId}: contour must list 4 edges, got ${f.edges.length}`);
    const seen = new Set();
    for (const eid of f.edges) {
      const e = edgeById.get(eid);
      if (!e) { fail("F-C5", `face ${surfId}: contour references unknown edge ${eid}`); continue; }
      if (seen.has(eid)) fail("F-C5", `face ${surfId}: contour lists edge ${eid} twice`);
      seen.add(eid);
      if (!e.s.includes(surfId))
        fail("F-C5", `face ${surfId}: edge ${eid}.s={${e.s}} does not include this face`);
      faceRefs.get(eid).push(surfId);
    }

    /* F-C1 / F-C1b plane frame */
    const c = cross(f.v1, f.v2);
    if (len(sub(c, f.v3)) > EPS)
      fail("F-C1", `face ${surfId}: v3=${f.v3} != v1 x v2 = [${c.map((x) => x.toFixed(3))}] (right-hand rule)`);
    if (Math.abs(len(f.v1) - 1) > EPS || Math.abs(len(f.v2) - 1) > EPS || Math.abs(len(f.v3) - 1) > EPS)
      fail("F-C1b", `face ${surfId}: v1/v2/v3 not unit (|v3|=${len(f.v3).toFixed(4)})`);
    if (Math.abs(dot(f.v1, f.v2)) > EPS)
      fail("F-C1b", `face ${surfId}: v1 not perpendicular to v2`);

    /* F-C3 coplanarity of origin + contour endpoints */
    for (const eid of f.edges) {
      const e = edgeById.get(eid);
      if (!e) continue;
      for (const p of [e.p1, e.p2]) {
        const d = dot(f.v3, sub(p, f.o));
        if (Math.abs(d) > EPS)
          fail("F-C3", `face ${surfId}: edge ${eid} endpoint ${p} off plane by ${d.toFixed(4)} (origin ${f.o}, normal v3)`);
      }
    }

    /* F-C6 material half-space over ALL solid vertices */
    for (const p of pts) {
      const d = f.orient * dot(f.v3, sub(p, f.o));
      if (d > EPS)
        fail("F-C6", `face ${surfId}: vertex ${p} violates orient*dot(v3,P-o)<=0 (got ${d.toFixed(4)}, orient=${f.orient})`);
    }

    /* F-G8 directed cycle closure (dir flag on THIS face) */
    const directed = [];
    let ok = true;
    for (const eid of f.edges) {
      const e = edgeById.get(eid);
      if (!e) { ok = false; continue; }
      const si = e.s.indexOf(surfId);
      if (si < 0) { ok = false; continue; } /* already reported by F-C5 */
      const flip = e.dir[si];
      if (flip !== 0 && flip !== 1) { fail("F-G8", `face ${surfId}: edge ${eid} dir=${flip} not in {0,1}`); ok = false; continue; }
      directed.push(flip === 0 ? [e.p1, e.p2, eid] : [e.p2, e.p1, eid]);
    }
    if (ok && directed.length === 4) {
      /* chain: start anywhere, follow endpoint->startpoint, must close in 4 hops */
      const used = new Set();
      let cur = directed[0];
      used.add(0);
      let chain = [cur[2]];
      for (let hop = 1; hop < 4; hop++) {
        const nxt = directed.findIndex((d, i) => !used.has(i) && key(d[0]) === key(cur[1]));
        if (nxt < 0) {
          fail("F-G8", `face ${surfId}: directed contour does not chain (stuck at edge ${cur[2]} end ${cur[1]})`);
          chain = null;
          break;
        }
        used.add(nxt);
        cur = directed[nxt];
        chain.push(cur[2]);
      }
      if (chain && key(cur[1]) !== key(directed[0][0]))
        fail("F-G8", `face ${surfId}: directed contour does not close (ends ${cur[1]}, starts ${directed[0][0]})`);
    } else if (!ok) {
      fail("F-G8", `face ${surfId}: directed-cycle check skipped (contour edges unusable)`);
    }
  });

  /* G-G2 / G-G3 / G-G5 edge usage */
  for (const e of edges) {
    const refs = faceRefs.get(e.id);
    if (refs.length !== 2)
      fail("G-G2", `edge ${e.id}: referenced by ${refs.length} face contours (need exactly 2; refs={${refs}})`);
    const sSet = [...e.s].sort((a, b) => a - b).join(",");
    const rSet = [...new Set(refs)].sort((a, b) => a - b).join(",");
    if (sSet !== rSet)
      fail("G-G5", `edge ${e.id}: s[]={${e.s}} != referencing faces {${[...new Set(refs)]}`);
    /* G-G7 orientability: dirs opposite on the two sides */
    if (e.dir[0] === e.dir[1])
      fail("G-G7", `edge ${e.id}: dir flags equal (${e.dir}) - shared edge must be traversed oppositely by its two faces`);
    if (e.dir.some((d) => d !== 0 && d !== 1))
      fail("G-G7", `edge ${e.id}: dir flag not in {0,1}`);
  }
  return fails;
}

/* ------------------------------------------------------------- selftest */

/* Historical ISSUE-L2 row + two synthetic mutations. PASS iff the checker
 * CATCHES each mutation with (at least) the expected check ids. */
const BUGGY_ROW = { orient: 1, v1: [1, 0, 0], v2: [0, 0, 1], v3: [0, 1, -1], o: [0, 10, 0], edges: [2, 9, 5, 10] };

function mutate(base, fn) {
  const t = {
    edges: base.edges.map((e) => ({ ...e, s: [...e.s], dir: [...e.dir], p1: [...e.p1], p2: [...e.p2] })),
    faces: base.faces.map((f) => ({ ...f, v1: [...f.v1], v2: [...f.v2], v3: [...f.v3], o: [...f.o], edges: [...f.edges] })),
  };
  fn(t);
  return t;
}

function runSelftest(base) {
  const mutations = [
    {
      name: "M1 historical ISSUE-L2 buggy Surface-2 row",
      build: () => mutate(base, (t) => { t.faces[1] = { ...BUGGY_ROW, v1: [...BUGGY_ROW.v1], v2: [...BUGGY_ROW.v2], v3: [...BUGGY_ROW.v3], o: [...BUGGY_ROW.o], edges: [...BUGGY_ROW.edges] }; }),
      expect: ["F-C1", "F-C3", "F-C5", "G-G2"],
    },
    {
      name: "M2 orient flip on face 6 (top)",
      build: () => mutate(base, (t) => { t.faces[5].orient = -1; }),
      expect: ["F-C6"],
    },
    {
      name: "M3 dir-pair flip on edge 7",
      build: () => mutate(base, (t) => { const e = t.edges.find((x) => x.id === 7); e.dir = [0, 1]; }),
      expect: ["F-G8"],
    },
  ];
  let bad = 0;
  const baseline = runChecks(base);
  if (baseline.length !== 0) {
    console.log(`[selftest] BASELINE unexpectedly fails (${baseline.length}) - fix the table first:`);
    baseline.forEach((f) => console.log(`  ${f}`));
    return 1;
  }
  console.log("[selftest] baseline table passes (0 failures)");
  for (const mu of mutations) {
    const fails = runChecks(mu.build());
    const hitIds = new Set(fails.map((f) => f.split(":")[0]));
    const missing = mu.expect.filter((id) => !hitIds.has(id));
    if (fails.length === 0 || missing.length > 0) {
      console.log(`[selftest] NOT CAUGHT: ${mu.name} (failures=${fails.length}, missing expected: ${missing.join(",") || "none"})`);
      bad++;
    } else {
      console.log(`[selftest] caught: ${mu.name} -> ${fails.length} failure(s) incl. ${mu.expect.join(",")}`);
    }
  }
  return bad === 0 ? 0 : 1;
}

/* ------------------------------------------------------------- main    */

const args = process.argv.slice(2);
const selftest = args.includes("--selftest");
const pos = args.filter((a) => !a.startsWith("--"));
const file = pos[0] || join(process.cwd(), "src", "demo3", "demo3_adapter.c");

let tables;
try {
  tables = parseTables(readFileSync(file, "utf8"));
} catch (e) {
  console.error(`[p3_geom] PARSE ERROR: ${e.message} (file: ${file})`);
  process.exit(2);
}

if (selftest) {
  const rc = runSelftest(tables);
  console.log(rc === 0 ? "=== P3 GEOM SELFTEST: PASS (all mutations caught) ===" : "=== P3 GEOM SELFTEST: FAIL ===");
  process.exit(rc);
}

const fails = runChecks(tables);
const nChecks = 13; /* F-C1,C1b,C3,C5,C6,G8 + G-G1,G2,G3(implicit),G4,G5,G7,G9 */
if (fails.length === 0) {
  console.log(`[p3_geom] ${file}`);
  console.log(`[p3_geom] 12 edges / 6 faces / 8 vertices parsed; ${nChecks} invariant families all hold`);
  console.log("=== P3 GEOM: PASS ===");
  process.exit(0);
}
console.log(`[p3_geom] ${file}`);
fails.forEach((f) => console.log(`  FAIL ${f}`));
console.log(`=== P3 GEOM: FAIL (${fails.length}) ===`);
process.exit(1);
