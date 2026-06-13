// ─── Element refs ────────────────────────────────────────────────────────────
const canvas        = document.querySelector("#graph");
const ctx           = canvas.getContext("2d");
const statsEl       = document.querySelector("#project-strip");
const searchEl      = document.querySelector("#search");
const candidateEl   = document.querySelector("#candidate-list");
const refreshEl     = document.querySelector("#refresh");
const todoFilterEl  = document.querySelector("#todo-filter");
const todoListEl    = document.querySelector("#todo-list");
const kindFilterEl  = document.querySelector("#kind-filter");
const methodSearchEl  = document.querySelector("#method-search");
const methodTabsEl    = document.querySelector("#method-tabs");
const methodListEl    = document.querySelector("#method-list");
const graphStatusEl   = document.querySelector("#graph-status");
const inspectorEl     = document.querySelector("#inspector");
const titleEl         = document.querySelector("#detail-title");
const overlineEl      = document.querySelector("#detail-overline");
const kindEl          = document.querySelector("#detail-kind");
const detailEl        = document.querySelector("#detail-list");
const neighborEl      = document.querySelector("#neighbor-list");

// ─── Palette ─────────────────────────────────────────────────────────────────
const PALETTE = {
  file:          "#8ab4f8",
  metadata:      "#aab8b2",
  context:       "#eef5f1",
  component:     "#52d6ff",
  observation:   "#52d6ff",
  inference:     "#d0bcff",
  decision:      "#ffb86b",
  documentation: "#b8f7d4",
  think:         "#c07ef0",
  idea:          "#f0a07e",
  fix:           "#ff6b6b",
  todo:          "#ffd400",
  done:          "#49d28f",
  test:          "#49d28f",
  "test-meta":   "#49d28f",
  source:        "#f4a261",
  method:        "#ffffff",
  type:          "#eef5f1",
  dependency:    "#52d6ff",
  dependent:     "#d0bcff",
};

// ─── State ────────────────────────────────────────────────────────────────────
let graph        = { nodes: [], edges: [], stats: {} };
let nodeById     = new Map();
let selected     = null;
let selectedEdge = null;
let cascade      = emptyCascade();
let activeKind   = "all";
let candidates   = [];
let candidateIdx = 0;
let activeMethodGroup = "core";
let viewMode     = "context";
let methodTree   = null;
let methodByName = new Map();

// ─── Pointer / drag ───────────────────────────────────────────────────────────
let pointer   = { x: 0, y: 0, worldX: 0, worldY: 0 };
let dragging  = false;
let dragStart = null;

// ─── Camera ───────────────────────────────────────────────────────────────────
let camera = { x: 0, y: 0, zoom: 0.82, tx: 0, ty: 0, targetZoom: 0.82, zoomAnchor: null };

// ─── Render / perf bookkeeping ────────────────────────────────────────────────
let lastFrame           = performance.now();
let pendingDoneAnimId   = "";
let frameRequested      = false;
let doneAnimationUntil  = 0;
let perfProbe           = null;
let graphLoaded         = false;
let searchMatchedIds    = new Set();

// ─── Offscreen (static graph layer) ──────────────────────────────────────────
/** We render edges + dimmed nodes onto an offscreen canvas whenever the graph
 *  changes, then blit it each frame and only re-draw the dynamic layer on top.
 *  This cuts per-frame work from O(nodes+edges) to O(highlight set). */
let offscreen      = null;   // OffscreenCanvas | null
let offscreenCtx   = null;
let offscreenDirty = true;   // must re-bake after graph/camera/filter changes

// ─── Render cache ─────────────────────────────────────────────────────────────
let renderCache = { graph: null, nodes: [], edges: [] };

// ─── Boot ─────────────────────────────────────────────────────────────────────
loadGraph();
scheduleFrame();
window.__visualizerPerf = runPerfProbe;

// ─── Event listeners ──────────────────────────────────────────────────────────
refreshEl.addEventListener("click", loadGraph);

todoFilterEl.addEventListener("change", () => {
  renderTodos();
  scheduleFrame();
});

methodSearchEl.addEventListener("input", () => {
  renderMethods();
  scheduleFrame();
});

methodTabsEl.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-group]");
  if (!btn) return;
  activeMethodGroup = btn.dataset.group;
  methodTabsEl.querySelectorAll("button").forEach((b) => b.classList.toggle("active", b === btn));
  renderMethods();
  scheduleFrame();
});

searchEl.addEventListener("input", () => {
  applyVisibility();
  updateCandidates();
  renderTodos();
  scheduleFrame();
});

searchEl.addEventListener("keydown", (e) => {
  if      (e.key === "ArrowDown"  ) { e.preventDefault(); moveCandidate(1); }
  else if (e.key === "ArrowUp"    ) { e.preventDefault(); moveCandidate(-1); }
  else if (e.key === "Enter" && candidates[candidateIdx]) {
    e.preventDefault();
    selectNode(candidates[candidateIdx], true);
  }
  else if (e.key === "Escape") { candidateEl.classList.remove("open"); }
});

searchEl.addEventListener("focus", updateCandidates);

kindFilterEl.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-kind]");
  if (!btn) return;
  activeKind = btn.dataset.kind;
  kindFilterEl.querySelectorAll("button").forEach((b) => b.classList.toggle("active", b === btn));
  applyVisibility();
  updateCandidates();
  scheduleFrame();
});

// ─── Canvas pointer events ────────────────────────────────────────────────────
canvas.addEventListener("pointerdown", (e) => {
  canvas.setPointerCapture(e.pointerId);
  dragging  = true;
  canvas.classList.add("dragging");
  dragStart = { x: e.clientX, y: e.clientY, cx: camera.tx, cy: camera.ty };
  scheduleFrame();
});

canvas.addEventListener("pointermove", (e) => {
  updatePointer(e);
  if (!dragging || !dragStart) return;
  const dx = (e.clientX - dragStart.x) / camera.zoom;
  const dy = (e.clientY - dragStart.y) / camera.zoom;
  camera.tx = dragStart.cx - dx;
  camera.ty = dragStart.cy - dy;
  camera.x  = camera.tx;
  camera.y  = camera.ty;
  camera.zoomAnchor = null;
  offscreenDirty = true;
  scheduleFrame();
});

canvas.addEventListener("pointerup", (e) => {
  updatePointer(e);
  canvas.releasePointerCapture(e.pointerId);
  canvas.classList.remove("dragging");
  const moved = dragStart && Math.hypot(e.clientX - dragStart.x, e.clientY - dragStart.y) > 5;
  dragging  = false;
  dragStart = null;
  if (moved) return;
  const hit = nearestNode(pointer.worldX, pointer.worldY);
  if (hit) { selectActiveNode(hit, true); return; }
  const edge = nearestEdge(pointer.worldX, pointer.worldY);
  if (edge) { selectEdge(edge); return; }
  deselectAll();
});

canvas.addEventListener("wheel", (e) => {
  e.preventDefault();
  updatePointer(e);
  const factor = Math.exp(-e.deltaY * 0.0012);
  camera.targetZoom = clamp(camera.targetZoom * factor, 0.28, 3.2);
  camera.zoomAnchor = {
    screenX: pointer.x,   screenY: pointer.y,
    worldX:  pointer.worldX, worldY: pointer.worldY,
  };
  offscreenDirty = true;
  scheduleFrame();
}, { passive: false });

// ─── Data loading ─────────────────────────────────────────────────────────────
async function loadGraph() {
  graphStatusEl.textContent = "syncing";
  const res     = await fetch("/api/context");
  const payload = await res.json();
  graph       = prepareGraph(payload);
  nodeById    = new Map(graph.nodes.map((n) => [n.id, n]));
  methodByName = new Map((graph.methods || []).map((m) => [m.name, m]));
  applyVisibility();
  renderStats();
  renderTodos();
  renderMethods();
  updateCandidates();
  const first = selected ? (nodeById.get(selected.id) || graph.nodes[0])
                         : (graph.nodes.find((n) => n.kind === "todo") || graph.nodes[0]);
  selectNode(first, false);
  graphStatusEl.textContent = `${graph.stats.objects} objects · ${graph.stats.openTodos} TODO`;
  graphLoaded = true;
  if (new URLSearchParams(window.location.search).has("perf")) runBrowserPerfReport();
}

function prepareGraph(payload) {
  const nodes = payload.nodes.map((node, i) => {
    const group  = groupIndex(node.kind);
    const angle  = i * 2.399963 + group * 0.51;
    const radius = 160 + group * 116 + (i % 17) * 4;
    const donePulseStarted =
      pendingDoneAnimId === node.id && node.status === "done" ? performance.now() : 0;
    if (donePulseStarted)
      doneAnimationUntil = Math.max(doneAnimationUntil, donePulseStarted + 900);
    return {
      ...node,
      x: Math.cos(angle) * radius,  y: Math.sin(angle) * radius,
      vx: 0, vy: 0,
      fx: Math.cos(angle) * radius, fy: Math.sin(angle) * radius,
      radius: node.kind === "todo" ? 15 : node.kind === "test" ? 8 : 10,
      visible: true,
      pulse:   (i * 0.79) % (Math.PI * 2),
      donePulseStarted,
      // pre-build search tokens here once
      _searchTokens: buildSearchTokens(node),
    };
  });
  pendingDoneAnimId = "";
  const edges = payload.edges.map((e, i) =>
    ({ ...e, id: `${e.source}->${e.target}:${e.kind}:${i}` }));
  invalidateRenderCache();
  offscreenDirty = true;
  return { ...payload, nodes, edges };
}

function groupIndex(kind) {
  return { todo:0, documentation:1, test:2, decision:3, observation:4,
           component:5, context:6, file:7, source:8, metadata:9,
           think:3.5, idea:3.7, fix:3.3 }[kind] ?? 6;
}

// ─── Visibility ───────────────────────────────────────────────────────────────
function applyVisibility() {
  viewMode     = "context";
  methodTree   = null;
  selectedEdge = null;
  cascade      = emptyCascade();
  const q = searchEl.value.trim();
  const matching   = new Set();
  const connected  = new Set();
  if (q) {
    for (const node of graph.nodes) {
      if (kindMatches(node) && nodeSearchScore(node, q) > -Infinity) matching.add(node.id);
    }
    for (const edge of graph.edges) {
      if (matching.has(edge.source)) connected.add(edge.target);
      if (matching.has(edge.target)) connected.add(edge.source);
    }
  }
  searchMatchedIds = matching;
  for (const node of graph.nodes) {
    node.visible = kindMatches(node) && (!q || matching.has(node.id) || connected.has(node.id));
  }
  invalidateRenderCache();
  offscreenDirty = true;
}

function kindMatches(node) {
  if (activeKind === "all" ) return true;
  if (activeKind === "todo") return node.kind === "todo" && node.status !== "done";
  if (activeKind === "done") return node.kind === "todo" && node.status === "done";
  if (activeKind === "context") return !["todo","test","decision","documentation","think","idea","fix"].includes(node.kind);
  return node.kind === activeKind;
}

// ─── Search ───────────────────────────────────────────────────────────────────
/**
 * Build a structured token object once per node at load time.
 * Tiers: identity (id, label, heading) > meta (kind, file, record_type) > body (summary, content).
 */
function buildSearchTokens(node) {
  return {
    id:      String(node.id      || "").toLowerCase(),
    label:   String(node.label   || "").toLowerCase(),
    heading: String(node.heading || "").toLowerCase(),
    kind:    String(node.kind    || "").toLowerCase(),
    file:    String(node.file    || "").toLowerCase(),
    record:  String(node.record_type || "").toLowerCase(),
    summary: String(node.summary || "").toLowerCase(),
    content: String(node.content || "").toLowerCase(),
  };
}

/**
 * Score a node against a query string.
 * Returns -Infinity for no-match.
 *
 * Scoring tiers (additive):
 *   exact match on identity field  +300
 *   starts-with on identity field  +180
 *   substring on identity field    +100
 *   fuzzy on identity field        +60
 *   substring on meta field        +40
 *   substring on body field        +12
 *   fuzzy on body field            +4
 *   kind bonus for todo/doc        +24 / +18
 */
function nodeSearchScore(node, rawQuery) {
  const q = rawQuery.toLowerCase().trim();
  if (!q) return 0;

  const t = node._searchTokens || buildSearchTokens(node);
  let score = node.kind === "todo" ? 24 : node.kind === "documentation" ? 18 : 0;

  // ── identity tier ──
  const identity = [t.id, t.label, t.heading];
  let identityHit = false;
  for (const field of identity) {
    if (!field) continue;
    if (field === q)            { score += 300; identityHit = true; }
    else if (field.startsWith(q)) { score += 180; identityHit = true; }
    else if (field.includes(q)) { score += 100; identityHit = true; }
    else {
      const fs = fuzzyFieldScore(field, q);
      if (fs > -Infinity) { score += 60 + fs; identityHit = true; }
    }
  }

  // ── meta tier ──
  const meta = [t.kind, t.file, t.record];
  for (const field of meta) {
    if (field && field.includes(q)) score += 40;
  }

  // ── body tier ──
  const body = [t.summary, t.content];
  let bodyHit = false;
  for (const field of body) {
    if (!field) continue;
    if (field.includes(q)) { score += 12; bodyHit = true; }
    else {
      const fs = fuzzyFieldScore(field, q);
      if (fs > -Infinity) { score += 4 + Math.max(0, fs * 0.3); bodyHit = true; }
    }
  }

  // must match something
  if (!identityHit && score <= 24 && !bodyHit) {
    const allText = [t.id, t.label, t.heading, t.kind, t.file, t.record, t.summary, t.content]
      .filter(Boolean).join(" ");
    const fs = fuzzyFieldScore(allText, q);
    if (fs === -Infinity) return -Infinity;
    score += fs;
  }

  return score;
}

/** Character-by-character fuzzy score on a single string. */
function fuzzyFieldScore(haystack, needle) {
  let h = 0, score = 0;
  for (let n = 0; n < needle.length; n++) {
    const found = haystack.indexOf(needle[n], h);
    if (found === -1) return -Infinity;
    score += found === h ? 8 : Math.max(1, 8 - (found - h));
    h = found + 1;
  }
  if (haystack.includes(needle)) score += 32;
  return score;
}

// ─── Candidates ───────────────────────────────────────────────────────────────
function updateCandidates() {
  const q    = searchEl.value.trim();
  const base = graph.nodes.filter(kindMatches);
  candidates = (q
    ? base.map((n) => ({ n, s: nodeSearchScore(n, q) }))
          .filter((x) => x.s > -Infinity)
          .sort((a, b) => b.s - a.s)
          .map((x) => x.n)
    : base
  ).slice(0, 12);
  candidateIdx = clamp(candidateIdx, 0, Math.max(0, candidates.length - 1));
  renderCandidates();
  if (candidates[candidateIdx]) focusCandidate(candidates[candidateIdx]);
}

function renderCandidates() {
  const open = searchEl.matches(":focus") && candidates.length > 0;
  candidateEl.classList.toggle("open", open);
  candidateEl.innerHTML = candidates.map((node, i) => `
    <div class="candidate ${node.kind === "todo" ? "todo" : ""} ${node.kind === "documentation" ? "documentation" : ""} ${i === candidateIdx ? "active" : ""}" data-index="${i}">
      <div class="candidate-kind">${escHtml(node.kind === "todo" ? "TODO" : node.kind)}</div>
      <div>
        <div class="candidate-title">${escHtml(node.label)}</div>
        <div class="candidate-file">${escHtml(node.heading || node.file)} · ${escHtml(node.file)}</div>
        <div class="candidate-snippet">${escHtml(trunc(node.summary || node.content || node.id, 140))}</div>
      </div>
    </div>`).join("");

  candidateEl.querySelectorAll(".candidate").forEach((el) => {
    el.addEventListener("mouseenter", () => {
      candidateIdx = Number(el.dataset.index);
      focusCandidate(candidates[candidateIdx]);
      renderCandidates();
    });
    el.addEventListener("click", () => selectNode(candidates[Number(el.dataset.index)], true));
  });
}

function moveCandidate(delta) {
  if (!candidates.length) return;
  candidateIdx = (candidateIdx + delta + candidates.length) % candidates.length;
  focusCandidate(candidates[candidateIdx]);
  renderCandidates();
}

function focusCandidate(node) {
  if (!node) return;
  viewMode     = "context";
  methodTree   = null;
  selectedEdge = null;
  selected     = node;
  cascade      = computeCascade([node.id]);
  camera.tx    = node.x;
  camera.ty    = node.y;
  camera.targetZoom = 1.65;
  offscreenDirty = true;
  updateInspector(node);
}

// ─── Stats / lists ────────────────────────────────────────────────────────────
function renderStats() {
  const done = graph.nodes.filter((n) => n.kind === "todo" && n.status === "done").length;
  const cards = [
    ["Objects",   graph.stats.objects,   ""],
    ["Morphisms", graph.stats.morphisms, ""],
    ["TODO",      graph.stats.openTodos, "todo-card"],
    ["DONE",      done,                  "done-card"],
  ];
  statsEl.innerHTML = cards.map(([label, val, cls]) =>
    `<article class="project-card ${cls}"><strong>${val}</strong><span>${label}</span></article>`
  ).join("");
}

function renderTodos() {
  const filter = todoFilterEl.value;
  const q = searchEl.value.trim();
  const todos = graph.nodes
    .filter((n) => n.kind === "todo")
    .filter((n) => filter === "all" || (filter === "done" ? n.status === "done" : n.status !== "done"))
    .filter((n) => !q || nodeSearchScore(n, q) > -Infinity)
    .sort((a, b) => Number(a.status === "done") - Number(b.status === "done") || a.label.localeCompare(b.label));

  todoListEl.innerHTML = todos.map((todo) => `
    <article class="todo-item ${todo.status === "done" ? "done" : ""}" data-id="${escAttr(todo.id)}">
      <p class="todo-title">${escHtml(todo.summary || todo.label || "TODO")}</p>
      <div class="todo-meta">${escHtml(todo.file)} · ${escHtml(todo.status || "open")}${todo.completed_at ? ` · ${escHtml(todo.completed_at)}` : ""}</div>
      <div class="todo-actions">
        <button data-status="open"    class="state-open    ${todo.status !== "done" && todo.status !== "blocked" ? "active" : ""}">Open</button>
        <button data-status="done"    class="state-done    ${todo.status === "done"    ? "active" : ""}">Done</button>
        <button data-status="blocked" class="state-blocked ${todo.status === "blocked" ? "active" : ""}">Blocked</button>
      </div>
    </article>`).join("");

  todoListEl.querySelectorAll(".todo-item").forEach((el) => {
    el.addEventListener("click", (e) => {
      const node = nodeById.get(el.dataset.id);
      if (node) selectNode(node, true);
      const btn = e.target.closest("button[data-status]");
      if (btn) updateTodo(el.dataset.id, btn.dataset.status);
    });
  });
}

function renderMethods() {
  const q = methodSearchEl.value.trim();
  const methods = (graph.methods || [])
    .filter((m) => m.group === activeMethodGroup)
    .map((m) => ({ m, s: methodScore(m, q) }))
    .filter((x) => x.s > -Infinity)
    .sort((a, b) => b.s - a.s || a.m.name.localeCompare(b.m.name))
    .slice(0, 80)
    .map((x) => x.m);

  methodListEl.innerHTML = methods.map((m, i) => `
    <article class="method-item" data-index="${i}">
      <strong>${escHtml(m.name)}</strong>
      <div class="method-signature">${escHtml(m.signature || "value")}</div>
      <div class="method-file">${escHtml(m.file)}:${m.line}</div>
    </article>`).join("");

  methodListEl.querySelectorAll(".method-item").forEach((el) => {
    el.addEventListener("click", () => showMethod(methods[Number(el.dataset.index)]));
  });
}

function methodScore(method, query) {
  if (!query) return 1;
  const q    = query.toLowerCase();
  const name = method.name.toLowerCase();
  const sig  = method.signature.toLowerCase();
  const file = method.file.toLowerCase();
  let score  = 0;
  if (name === q)          score += 220;
  if (name.includes(q))    score += 90;
  if (sig.includes(q))     score += 82;
  const best = Math.max(
    fuzzyFieldScore(name, q) + 46,
    fuzzyFieldScore(sig,  q) + 26,
    fuzzyFieldScore(file, q),
  );
  if (best === -Infinity && score === 0) return -Infinity;
  return score + Math.max(0, best);
}

// ─── Method panel / tree ─────────────────────────────────────────────────────
function showMethod(method) {
  if (!method) return;
  methodTree = buildMethodTree(method);
  invalidateRenderCache();
  offscreenDirty = true;
  viewMode     = "method";
  selected     = null;
  selectedEdge = methodTree.edges[0] || null;
  cascade = selectedEdge
    ? computeCascade([selectedEdge.source, selectedEdge.target], selectedEdge.id)
    : emptyCascade();
  camera.tx = 0; camera.ty = 0; camera.targetZoom = 1.05; camera.zoomAnchor = null;
  inspectorEl.classList.remove("todo-selected");
  overlineEl.textContent = method.group === "prelude" ? "Prelude Method" : "Core Method";
  titleEl.textContent    = method.name;
  kindEl.textContent     = `${method.group} · ${method.form}`;
  detailEl.innerHTML = [
    ["Signature", method.signature || "value"],
    ["File",      `${method.file}:${method.line}`],
    ["Uses",      method.uses.length    ? method.uses.join(", ")    : "No parsed core calls."],
    ["Used by",   method.used_by.length ? method.used_by.join(", ") : "No parsed callers."],
    ["Open",      openFileButton(method), true],
  ].map(renderDetailRow).join("");
  bindOpenButtons(method);
  neighborEl.innerHTML = methodTree.edges.slice(1, 11).map((e) =>
    `<div class="neighbor" data-id="${escAttr(e.id)}"><strong>${escHtml(e.label)}</strong><span>${escHtml(e.kind)} · ${escHtml(e.method?.file || "")}</span></div>`
  ).join("");
  neighborEl.querySelectorAll(".neighbor").forEach((el) => {
    el.addEventListener("click", () => {
      const e = methodTree.edges.find((x) => x.id === el.dataset.id);
      if (e) selectEdge(e);
    });
  });
}

function buildMethodTree(method) {
  const nodes = [], edges = [];
  addMethodArrow(method, "method", 0, 0, nodes, edges);
  addMethodRing(method.uses    || [], "dependency", -1, method, nodes, edges);
  addMethodRing(method.used_by || [], "dependent",   1, method, nodes, edges);
  return { nodes, edges };
}

function addMethodRing(names, kind, side, root, nodes, edges) {
  const count = Math.max(1, names.length);
  names.forEach((name, i) => {
    const src = methodByName.get(name)
      || { id: `method:external:${name}`, name, signature: "", group: "external", file: "external" };
    addMethodArrow(src, kind, side * 310, (i - (count - 1) / 2) * 72, nodes, edges);
  });
}

function addMethodArrow(method, kind, x, y, nodes, edges) {
  const parts  = signatureParts(method.signature || "");
  const source = makeTypeNode(`${method.id}:source`, parts.source, method.file, x - 76, y, nodes.length);
  const target = makeTypeNode(`${method.id}:target`, parts.target, method.file, x + 76, y, nodes.length + 1);
  nodes.push(source, target);
  edges.push({
    id:     `${kind}:${method.id}`,
    source: source.id, target: target.id,
    label:  method.name,
    kind:   kind === "method" ? "function" : kind,
    method,
  });
}

function makeTypeNode(id, label, file, x, y, index) {
  return {
    id, label, kind: "type", file, summary: "type object",
    x, y, fx: x, fy: y, vx: 0, vy: 0,
    radius: 13, visible: true,
    pulse: index * 0.8,
    _searchTokens: buildSearchTokens({ id, label, kind: "type", file, summary: "type object" }),
  };
}

function signatureParts(signature) {
  const parts = String(signature || "value").split("->").map((p) => p.trim()).filter(Boolean);
  if (parts.length < 2) return { source: "Unit", target: parts[0] || "Value" };
  return { source: parts.slice(0, -1).join(" -> "), target: parts[parts.length - 1] };
}

// ─── TODO write-back ──────────────────────────────────────────────────────────
async function updateTodo(id, status) {
  graphStatusEl.textContent = "writing TODO";
  pendingDoneAnimId = status === "done" ? id : "";
  await fetch("/api/todo", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ id, status }),
  });
  await loadGraph();
}

// ─── Frame loop ───────────────────────────────────────────────────────────────
function frame(now) {
  frameRequested = false;
  const dt    = Math.min(0.04, (now - lastFrame) / 1000);
  lastFrame   = now;
  const t0    = performance.now();
  resizeCanvas();
  const moving = updateCamera(dt);
  if (moving || offscreenDirty) {
    offscreenDirty = true;   // camera moved → bake again
  }
  draw(now / 1000);
  if (perfProbe) recordPerfFrame(performance.now() - t0);
  if (moving || dragging || now < doneAnimationUntil) scheduleFrame();
}

function scheduleFrame() {
  if (frameRequested) return;
  frameRequested = true;
  requestAnimationFrame(frame);
}

// ─── Perf probe ───────────────────────────────────────────────────────────────
function runPerfProbe(frames = 180) {
  perfProbe = { frames: [], remaining: frames, resolve: null };
  const p = new Promise((r) => { perfProbe.resolve = r; });
  scheduleFrame();
  return p;
}

function recordPerfFrame(dur) {
  perfProbe.frames.push(dur);
  if (--perfProbe.remaining > 0) { scheduleFrame(); return; }
  const sorted = [...perfProbe.frames].sort((a, b) => a - b);
  const result = {
    frames: sorted.length,
    avg:    sorted.reduce((s, v) => s + v, 0) / sorted.length,
    p95:    sorted[Math.floor(sorted.length * 0.95)],
    max:    sorted[sorted.length - 1],
  };
  perfProbe.resolve(result);
  perfProbe = null;
}

async function runBrowserPerfReport() {
  await new Promise((r) => setTimeout(r, 120));
  const result = await runPerfProbe(180);
  await fetch("/api/perf", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      ...result,
      userAgent: navigator.userAgent,
      nodes: getRenderCache().nodes.length,
      edges: getRenderCache().edges.length,
      timestamp: new Date().toISOString(),
    }),
  });
}

// ─── Canvas resize ────────────────────────────────────────────────────────────
function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const w   = Math.floor(window.innerWidth  * dpr);
  const h   = Math.floor(window.innerHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width  = w;
    canvas.height = h;
    offscreenDirty = true;
    // resize offscreen too
    if (offscreen) { offscreen.width = w; offscreen.height = h; }
  }
}

// ─── Camera ───────────────────────────────────────────────────────────────────
function updateCamera(dt) {
  const prev = { x: camera.x, y: camera.y, z: camera.zoom };
  const ease = 1 - Math.exp(-dt * 26);
  const zEase = 1 - Math.exp(-dt * 30);
  camera.zoom += (camera.targetZoom - camera.zoom) * zEase;
  if (camera.zoomAnchor) {
    camera.tx = camera.zoomAnchor.worldX - (camera.zoomAnchor.screenX - window.innerWidth  / 2) / camera.targetZoom;
    camera.ty = camera.zoomAnchor.worldY - (camera.zoomAnchor.screenY - window.innerHeight / 2) / camera.targetZoom;
    if (Math.abs(camera.zoom - camera.targetZoom) < 0.002) camera.zoomAnchor = null;
  }
  camera.x += (camera.tx - camera.x) * ease;
  camera.y += (camera.ty - camera.y) * ease;
  return (
    Math.abs(camera.x - prev.x) > 0.01 || Math.abs(camera.y - prev.y) > 0.01 ||
    Math.abs(camera.zoom - prev.z) > 0.0005 ||
    Math.abs(camera.x - camera.tx) > 0.01 || Math.abs(camera.y - camera.ty) > 0.01 ||
    Math.abs(camera.zoom - camera.targetZoom) > 0.0005
  );
}

// ─── Draw ─────────────────────────────────────────────────────────────────────
function draw(time) {
  const dpr    = window.devicePixelRatio || 1;
  const W      = canvas.width  / dpr;
  const H      = canvas.height / dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  // background
  const bg = ctx.createRadialGradient(W/2, H/2, 20, W/2, H/2, Math.max(W, H));
  bg.addColorStop(0, "#162024");
  bg.addColorStop(1, "#0d1113");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, W, H);

  drawGrid(W, H);

  // ── offscreen bake pass ──
  // Only re-bake the static layer when graph or camera changes.
  if (offscreenDirty) {
    bakeOffscreen(W, H, dpr, time);
    offscreenDirty = false;
  }

  // Blit the baked static layer
  if (offscreen) ctx.drawImage(offscreen, 0, 0, W, H);

  // ── dynamic layer: highlighted nodes only ──
  ctx.save();
  ctx.translate(W / 2, H / 2);
  ctx.scale(camera.zoom, camera.zoom);
  ctx.translate(-camera.x, -camera.y);

  const cache  = getRenderCache();
  const bounds = visibleWorldBounds(W, H, 80);
  const hasSelection = selected || selectedEdge;

  // Re-draw selected edges on top with full opacity
  for (const item of cache.edges) {
    const edgeSel  = selectedEdge && selectedEdge.id === item.edge.id;
    const connSel  = selected && (item.edge.source === selected.id || item.edge.target === selected.id);
    const inCascade = cascade.edges.has(item.edge.id);
    if (!edgeSel && !connSel && !inCascade) continue;
    if (!segmentInBounds(item.source, item.target, bounds)) continue;
    drawEdge(item.source, item.target, item.edge, time, true);
  }

  // Re-draw highlighted nodes on top
  for (const node of cache.nodes) {
    const isSel     = selected && selected.id === node.id;
    const isHit     = searchMatchedIds.has(node.id);
    const inCascade = cascade.nodes.has(node.id);
    if (!isSel && !isHit && !inCascade && hasSelection) continue;
    if (!pointInBounds(node, bounds)) continue;
    drawNode(node, time, true);
  }

  ctx.restore();
}

/**
 * Bake the full graph (all visible edges + all nodes at base opacity/dim)
 * onto an offscreen canvas. Re-run only when the graph or camera changes.
 */
function bakeOffscreen(W, H, dpr, time) {
  if (!offscreen || offscreen.width !== Math.floor(W * dpr) || offscreen.height !== Math.floor(H * dpr)) {
    if (typeof OffscreenCanvas !== "undefined") {
      offscreen    = new OffscreenCanvas(Math.floor(W * dpr), Math.floor(H * dpr));
      offscreenCtx = offscreen.getContext("2d");
    } else {
      // fallback: second canvas element
      offscreen = document.createElement("canvas");
      offscreen.width  = Math.floor(W * dpr);
      offscreen.height = Math.floor(H * dpr);
      offscreenCtx = offscreen.getContext("2d");
    }
  }

  const oc = offscreenCtx;
  oc.clearRect(0, 0, offscreen.width, offscreen.height);
  oc.setTransform(dpr, 0, 0, dpr, 0, 0);
  oc.save();
  oc.translate(W / 2, H / 2);
  oc.scale(camera.zoom, camera.zoom);
  oc.translate(-camera.x, -camera.y);

  const cache  = getRenderCache();
  const bounds = visibleWorldBounds(W, H, 80);
  const hasSelection = selected || selectedEdge;

  // All edges at dim/base opacity
  for (const item of cache.edges) {
    if (!segmentInBounds(item.source, item.target, bounds)) continue;
    const edgeSel   = selectedEdge && selectedEdge.id === item.edge.id;
    const connSel   = selected && (item.edge.source === selected.id || item.edge.target === selected.id);
    const inCascade = cascade.edges.has(item.edge.id);
    // skip bright edges — they'll be redrawn on the dynamic layer
    if (edgeSel || connSel || inCascade) continue;
    drawEdgeOn(oc, item.source, item.target, item.edge, time, false, hasSelection);
  }

  // All nodes at dim/base opacity (highlighted ones redrawn on dynamic layer)
  for (const node of cache.nodes) {
    if (!pointInBounds(node, bounds)) continue;
    const isSel     = selected && selected.id === node.id;
    const isHit     = searchMatchedIds.has(node.id);
    const inCascade = cascade.nodes.has(node.id);
    if (isSel || isHit || inCascade) continue;
    drawNodeOn(oc, node, time, false, hasSelection);
  }

  oc.restore();
}

// ─── Isolation alpha ──────────────────────────────────────────────────────────
/**
 * When something is selected, non-cascade items are pushed to a very low
 * alpha so the highlighted subgraph reads instantly. This is the core of the
 * "hide the rest" behaviour.
 */
const ISO_DIM = 0.07;   // alpha for unrelated nodes when selection is active

// ─── Edge drawing ─────────────────────────────────────────────────────────────
function drawEdge(a, b, edge, time, highlight) {
  drawEdgeOn(ctx, a, b, edge, time, highlight, !!(selected || selectedEdge));
}

function drawEdgeOn(c, a, b, edge, time, highlight, hasSelection) {
  const edgeSel  = selectedEdge && selectedEdge.id === edge.id;
  const connSel  = selected && (edge.source === selected.id || edge.target === selected.id);
  const inCasc   = cascade.edges.has(edge.id);

  let alpha;
  if      (edgeSel)   alpha = 0.92;
  else if (connSel)   alpha = 0.74;
  else if (inCasc)    alpha = 0.48;
  else                alpha = hasSelection ? ISO_DIM : 0.10;

  const mx = (a.x + b.x) / 2;
  const my = (a.y + b.y) / 2;
  const cx = mx - (b.y - a.y) * 0.08;
  const cy = my + (b.x - a.x) * 0.08;
  const end = trimEndpoint(cx, cy, b.x, b.y, b.radius + 4);

  c.beginPath();
  c.moveTo(a.x, a.y);
  c.quadraticCurveTo(cx, cy, end.x, end.y);
  c.strokeStyle = edgeColor(edge, alpha);
  c.lineWidth   = edgeSel ? 4 : connSel ? 2.6 : 1.1;
  c.stroke();
  drawArrowheadOn(c, cx, cy, end.x, end.y, c.strokeStyle, edgeSel || connSel ? 9 : 7);

  if (edgeSel || edge.method || connSel) {
    c.save();
    c.fillStyle    = edge.kind === "function" ? "#ffffff" : "#eef5f1";
    c.font         = `${edgeSel ? 800 : 650} 12px Inter, Roboto, sans-serif`;
    c.textAlign    = "center";
    c.textBaseline = "middle";
    c.shadowBlur   = 16;
    c.shadowColor  = "rgba(0,0,0,0.65)";
    c.fillText(trunc(edge.label, 28), cx, cy - 10);
    c.restore();
  }
}

function edgeColor(edge, alpha) {
  if (edge.kind === "verifies"  ) return `rgba(73,210,143,${alpha})`;
  if (edge.kind === "evidence"  ) return `rgba(255,212,0,${alpha})`;
  if (edge.kind === "function"  ) return `rgba(255,255,255,${Math.max(alpha, 0.72)})`;
  if (edge.kind === "dependency") return `rgba(82,214,255,${Math.max(alpha, 0.46)})`;
  if (edge.kind === "dependent" ) return `rgba(208,188,255,${Math.max(alpha, 0.46)})`;
  if (edge.kind === "uses"      ) return `rgba(82,214,255,${Math.max(alpha, 0.46)})`;
  if (edge.kind === "used-by"   ) return `rgba(208,188,255,${Math.max(alpha, 0.46)})`;
  return `rgba(238,245,241,${alpha})`;
}

function trimEndpoint(fx, fy, tx, ty, amt) {
  const dx = tx - fx, dy = ty - fy;
  const d  = Math.max(1, Math.hypot(dx, dy));
  return { x: tx - (dx / d) * amt, y: ty - (dy / d) * amt };
}

function drawArrowheadOn(c, fx, fy, tx, ty, color, size) {
  const angle = Math.atan2(ty - fy, tx - fx);
  c.save();
  c.fillStyle = color;
  c.beginPath();
  c.moveTo(tx, ty);
  c.lineTo(tx - Math.cos(angle - 0.48) * size, ty - Math.sin(angle - 0.48) * size);
  c.lineTo(tx - Math.cos(angle + 0.48) * size, ty - Math.sin(angle + 0.48) * size);
  c.closePath();
  c.fill();
  c.restore();
}

// ─── Node drawing ─────────────────────────────────────────────────────────────
function drawNode(node, time, highlight) {
  drawNodeOn(ctx, node, time, highlight, !!(selected || selectedEdge));
}

function drawNodeOn(c, node, time, highlight, hasSelection) {
  const isSel     = selected && selected.id === node.id;
  const isHit     = searchMatchedIds.has(node.id);
  const inCascade = cascade.nodes.has(node.id);
  const color     = nodeColor(node);
  const r = node.radius + (isSel ? 7 : isHit ? 4 : 0);

  let alpha;
  if      (isSel)      alpha = 1;
  else if (isHit)      alpha = 1;
  else if (inCascade)  alpha = 0.94;
  else if (!hasSelection) alpha = node.kind === "todo" ? 0.86 : 0.54;
  else                 alpha = ISO_DIM;

  c.save();
  c.shadowBlur  = isSel ? 18 : 0;
  if (isSel) c.shadowColor = color;
  c.beginPath();
  c.arc(node.x, node.y, Math.max(3, r), 0, Math.PI * 2);
  c.fillStyle   = color;
  c.globalAlpha = alpha;
  c.fill();
  c.globalAlpha = 1;
  c.shadowBlur  = 0;
  c.strokeStyle = isSel || isHit
    ? "#ffffff"
    : inCascade
      ? "rgba(255,255,255,0.72)"
      : node.kind === "todo"
        ? "rgba(255,244,184,0.72)"
        : "rgba(255,255,255,0.30)";
  c.lineWidth = isSel ? 3.4 : isHit ? 2.8 : inCascade ? 2.1 : node.kind === "todo" ? 1.6 : 1;
  c.globalAlpha = alpha;
  c.stroke();
  c.globalAlpha = 1;

  if (isSel || node.kind === "todo") {
    c.font      = `${node.kind === "todo" ? 700 : 500} 13px Inter, Roboto, sans-serif`;
    c.fillStyle = node.kind === "todo" ? "#ffe66d" : "#eef5f1";
    c.globalAlpha = alpha;
    c.textAlign = "center";
    c.fillText(trunc(node.label, 30), node.x, node.y - r - 12);
    c.globalAlpha = 1;
  }
  c.restore();
}

function nodeColor(node) {
  if (node.kind !== "todo" || node.status !== "done") return PALETTE[node.kind] || PALETTE.context;
  if (!node.donePulseStarted) return PALETTE.done;
  const t = clamp((performance.now() - node.donePulseStarted) / 850, 0, 1);
  return lerpHex(PALETTE.todo, PALETTE.done, easeOutCubic(t));
}

function lerpHex(a, b, t) {
  const av = hexToRgb(a), bv = hexToRgb(b);
  return `rgb(${Math.round(av.r+(bv.r-av.r)*t)},${Math.round(av.g+(bv.g-av.g)*t)},${Math.round(av.b+(bv.b-av.b)*t)})`;
}

function hexToRgb(v) {
  const h = v.replace("#","");
  return { r:parseInt(h,16)>>16&255, g:parseInt(h,16)>>8&255, b:parseInt(h,16)&255 };
}

function easeOutCubic(t) { return 1 - Math.pow(1 - t, 3); }

// ─── Grid ─────────────────────────────────────────────────────────────────────
function drawGrid(W, H) {
  ctx.save();
  const tl = screenToWorld(0, 0);
  const br = screenToWorld(W, H);
  const minorStep = niceGridStep(64 / camera.zoom);
  const majorStep = minorStep * 5;
  drawGridLines(tl.x, br.x, tl.y, br.y, minorStep, W, H, "rgba(238,245,241,0.045)", 1);
  drawGridLines(tl.x, br.x, tl.y, br.y, majorStep, W, H, "rgba(238,245,241,0.11)",  1.2);
  drawAxis(W, H);
  ctx.restore();
}

function drawGridLines(minX, maxX, minY, maxY, step, W, H, color, lw) {
  ctx.strokeStyle = color;
  ctx.lineWidth   = lw;
  const sx = Math.floor(minX / step) * step;
  for (let x = sx; x <= maxX; x += step) {
    const px = worldToScreenX(x, W);
    crispLine(px, 0, px, H);
  }
  const sy = Math.floor(minY / step) * step;
  for (let y = sy; y <= maxY; y += step) {
    const py = worldToScreenY(y, H);
    crispLine(0, py, W, py);
  }
}

function drawAxis(W, H) {
  ctx.lineWidth   = 1.4;
  ctx.strokeStyle = "rgba(82,214,255,0.22)";
  const x = worldToScreenX(0, W);
  if (x >= 0 && x <= W) crispLine(x, 0, x, H);
  ctx.strokeStyle = "rgba(73,210,143,0.22)";
  const y = worldToScreenY(0, H);
  if (y >= 0 && y <= H) crispLine(0, y, W, y);
}

function crispLine(x1, y1, x2, y2) {
  ctx.beginPath();
  ctx.moveTo(Math.round(x1) + 0.5, Math.round(y1) + 0.5);
  ctx.lineTo(Math.round(x2) + 0.5, Math.round(y2) + 0.5);
  ctx.stroke();
}

function niceGridStep(target) {
  const p = Math.pow(10, Math.floor(Math.log10(target)));
  const n = target / p;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * p;
}

// ─── Coordinate transforms ────────────────────────────────────────────────────
function screenToWorld(x, y) {
  return {
    x: (x - window.innerWidth  / 2) / camera.zoom + camera.x,
    y: (y - window.innerHeight / 2) / camera.zoom + camera.y,
  };
}

function worldToScreenX(x, W = window.innerWidth ) { return (x - camera.x) * camera.zoom + W / 2; }
function worldToScreenY(y, H = window.innerHeight) { return (y - camera.y) * camera.zoom + H / 2; }

function visibleWorldBounds(W, H, margin) {
  const tl = screenToWorld(-margin, -margin);
  const br = screenToWorld(W + margin, H + margin);
  return { minX: tl.x, minY: tl.y, maxX: br.x, maxY: br.y };
}

function pointInBounds(n, b)   { return n.x >= b.minX && n.x <= b.maxX && n.y >= b.minY && n.y <= b.maxY; }
function segmentInBounds(a, b, r) {
  return Math.max(a.x, b.x) >= r.minX && Math.min(a.x, b.x) <= r.maxX &&
         Math.max(a.y, b.y) >= r.minY && Math.min(a.y, b.y) <= r.maxY;
}

// ─── Hit testing ──────────────────────────────────────────────────────────────
function nearestNode(x, y) {
  let best = null, bestDist = Infinity;
  for (const node of getRenderCache().nodes) {
    const d = Math.hypot(node.x - x, node.y - y);
    if (d < node.radius + 16 && d < bestDist) { best = node; bestDist = d; }
  }
  return best;
}

function nearestEdge(x, y) {
  let best = null, bestDist = Infinity;
  for (const item of getRenderCache().edges) {
    const d = distToSegment(x, y, item.source.x, item.source.y, item.target.x, item.target.y);
    if (d < 16 / camera.zoom && d < bestDist) { best = item.edge; bestDist = d; }
  }
  return best;
}

function distToSegment(px, py, ax, ay, bx, by) {
  const dx = bx - ax, dy = by - ay;
  const len2 = dx*dx + dy*dy || 1;
  const t  = clamp(((px-ax)*dx + (py-ay)*dy) / len2, 0, 1);
  return Math.hypot(px - (ax + dx*t), py - (ay + dy*t));
}

// ─── Selection ────────────────────────────────────────────────────────────────
function selectNode(node, animate = true) {
  if (!node) return;
  viewMode     = "context";
  methodTree   = null;
  selected     = node;
  selectedEdge = null;
  cascade      = computeCascade([node.id]);
  camera.tx    = node.x;
  camera.ty    = node.y;
  camera.targetZoom = animate ? 1.72 : 0.9;
  offscreenDirty = true;
  updateInspector(node);
  scheduleFrame();
}

function selectActiveNode(node, animate = true) {
  if (viewMode === "method") {
    selected     = node;
    selectedEdge = null;
    cascade      = computeCascade([node.id]);
    camera.tx    = node.x;
    camera.ty    = node.y;
    camera.targetZoom = animate ? 1.35 : 1.05;
    offscreenDirty = true;
    scheduleFrame();
    return;
  }
  selectNode(node, animate);
}

function selectEdge(edge) {
  if (!edge) return;
  selectedEdge   = edge;
  selected       = null;
  cascade        = computeCascade([edge.source, edge.target], edge.id);
  offscreenDirty = true;
  updateEdgeInspector(edge);
  scheduleFrame();
}

function deselectAll() {
  selected       = null;
  selectedEdge   = null;
  cascade        = emptyCascade();
  offscreenDirty = true;
  overlineEl.textContent = "No Selection";
  titleEl.textContent    = "None";
  kindEl.textContent     = "No selection";
  detailEl.innerHTML     = "";
  neighborEl.innerHTML   = "";
  scheduleFrame();
}

// ─── Cascade ─────────────────────────────────────────────────────────────────
function emptyCascade() {
  return { nodes: new Set(), edges: new Set(), upstream: new Set(), downstream: new Set() };
}

function computeCascade(seedIds, seedEdgeId = "") {
  const active   = activeGraph();
  const outgoing = new Map();
  const incoming = new Map();
  for (const e of active.edges) {
    if (!outgoing.has(e.source)) outgoing.set(e.source, []);
    if (!incoming.has(e.target)) incoming.set(e.target, []);
    outgoing.get(e.source).push(e);
    incoming.get(e.target).push(e);
  }
  const down  = walkCascade(seedIds, outgoing, "target");
  const up    = walkCascade(seedIds, incoming, "source");
  const nodes = new Set([...seedIds, ...down.nodes, ...up.nodes]);
  const edges = new Set([...down.edges, ...up.edges]);
  if (seedEdgeId) edges.add(seedEdgeId);
  return { nodes, edges, upstream: up.nodes, downstream: down.nodes };
}

function walkCascade(seedIds, adj, nextKey) {
  const nodes = new Set(), edges = new Set();
  const queue = [...seedIds], seen = new Set(seedIds);
  while (queue.length && seen.size < 512) {
    const id = queue.shift();
    for (const e of adj.get(id) || []) {
      edges.add(e.id);
      const next = e[nextKey];
      if (seen.has(next)) continue;
      seen.add(next); nodes.add(next); queue.push(next);
    }
  }
  return { nodes, edges };
}

// ─── Render cache ─────────────────────────────────────────────────────────────
function activeGraph() {
  return viewMode === "method" && methodTree ? methodTree : graph;
}

function invalidateRenderCache() {
  renderCache.graph = null;
  offscreenDirty = true;
}

function getRenderCache() {
  const active = activeGraph();
  if (renderCache.graph === active) return renderCache;
  const nodes = active.nodes.filter((n) => n.visible);
  const byId  = active === graph ? nodeById : new Map(active.nodes.map((n) => [n.id, n]));
  const edges = active.edges
    .map((e) => ({ edge: e, source: byId.get(e.source), target: byId.get(e.target) }))
    .filter((x) => x.source && x.target && x.source.visible && x.target.visible);
  renderCache = { graph: active, nodes, edges };
  return renderCache;
}

// ─── Inspector ────────────────────────────────────────────────────────────────
function updateInspector(node) {
  inspectorEl.classList.toggle("todo-selected", node.kind === "todo");
  inspectorEl.classList.toggle("doc-selected",  node.kind === "documentation");
  overlineEl.textContent = node.kind === "todo" ? "TODO"
    : node.kind === "documentation" ? "Documentation"
    : "Selected Object";
  titleEl.textContent = node.label;
  kindEl.textContent  = `${node.record_type || (node.kind === "todo" ? "TODO" : node.kind)} · ${node.file}`;
  detailEl.innerHTML  = [
    ["ID",         node.id],
    ["Heading",    node.heading || node.label],
    ["Summary",    node.summary || "No summary recorded."],
    ["Record",     node.record_type || node.kind],
    ["Source",     node.source     || "not specified"],
    ["Confidence", node.confidence || "not specified"],
    ["Status",     node.status     || "active"],
    ["Outgoing",   graph.edges.filter((e) => e.source === node.id).length],
    ["Incoming",   graph.edges.filter((e) => e.target === node.id).length],
    ["Cascade",    `${cascade.nodes.size} objects · ${cascade.edges.size} morphisms`],
    ["Open",       openFileButton(node), true],
  ].filter(([, v]) => v !== "").map(renderDetailRow).join("");
  bindOpenButtons(node);
  renderNeighbors(node);
  writeOrgPane(node.content || node.summary || "");
}

function updateEdgeInspector(edge) {
  const active = activeGraph();
  const src    = findActiveNode(active, edge.source);
  const tgt    = findActiveNode(active, edge.target);
  inspectorEl.classList.remove("todo-selected");
  overlineEl.textContent = edge.method ? "Function Morphism" : "Selected Morphism";
  titleEl.textContent    = edge.method ? edge.method.name : edge.label;
  kindEl.textContent     = `${edge.kind} · ${src?.label || edge.source} -> ${tgt?.label || edge.target}`;
  detailEl.innerHTML = [
    ["Arrow",     edge.label],
    ["Source",    src ? `${src.label} · ${src.file}` : edge.source],
    ["Target",    tgt ? `${tgt.label} · ${tgt.file}` : edge.target],
    ["Signature", edge.method?.signature || "typed relation"],
    ["Cascade",   `${cascade.nodes.size} objects · ${cascade.edges.size} morphisms`],
    ["Open",      openFileButton(edge.method || src || tgt), true],
  ].map(renderDetailRow).join("");
  bindOpenButtons(edge.method || src || tgt);
  neighborEl.innerHTML = [src, tgt].filter(Boolean).map((n) =>
    `<div class="neighbor" data-id="${escAttr(n.id)}"><strong>${escHtml(n.label)}</strong><span>${escHtml(n.kind)} · ${escHtml(n.file)}</span></div>`
  ).join("");
  neighborEl.querySelectorAll(".neighbor").forEach((el) => {
    el.addEventListener("click", () => {
      const n = findActiveNode(activeGraph(), el.dataset.id);
      if (n) selectActiveNode(n, true);
    });
  });
}

function renderNeighbors(node) {
  const cascNodes = [...cascade.nodes]
    .filter((id) => id !== node.id)
    .map((id) => findActiveNode(activeGraph(), id))
    .filter(Boolean)
    .slice(0, 8);

  const immediate = graph.edges
    .filter((e) => e.source === node.id || e.target === node.id)
    .slice(0, Math.max(0, 10 - cascNodes.length))
    .map((e) => {
      const otherId = e.source === node.id ? e.target : e.source;
      const other   = nodeById.get(otherId);
      if (!other) return "";
      return `<div class="neighbor" data-id="${escAttr(other.id)}"><strong>${escHtml(trunc(other.label, 44))}</strong><span>${escHtml(e.label)} · ${escHtml(other.kind === "todo" ? "TODO" : other.kind)}</span></div>`;
    });

  neighborEl.innerHTML = [
    ...cascNodes.map((other) =>
      `<div class="neighbor" data-id="${escAttr(other.id)}"><strong>${escHtml(trunc(other.label, 44))}</strong><span>cascade · ${escHtml(other.kind === "todo" ? statusLabel(other) : other.kind)}</span></div>`),
    ...immediate,
  ].join("");

  neighborEl.querySelectorAll(".neighbor").forEach((el) => {
    el.addEventListener("click", () => selectNode(nodeById.get(el.dataset.id), true));
  });
}

function findActiveNode(active, id) {
  return active === graph ? nodeById.get(id) : active.nodes.find((n) => n.id === id);
}

// ─── Detail helpers ───────────────────────────────────────────────────────────
function renderDetailRow([key, value, html = false]) {
  return `<dt>${escHtml(key)}</dt><dd>${html ? value : escHtml(String(value))}</dd>`;
}

/**
 * Org-mode mini-renderer.
 *
 * Handles (in order of precedence):
 *   #+TITLE / #+KEYWORD lines  → skipped / used as metadata
 *   :PROPERTIES: … :END:       → skipped
 *   #+BEGIN_SRC … #+END_SRC    → <pre><code> block
 *   #+BEGIN_EXAMPLE … #+END   → <pre> block
 *   #+BEGIN_QUOTE … #+END      → <blockquote>
 *   * Heading (depth 1-3)      → <h2>–<h4>
 *   - / + / 1. list items      → <ul>/<ol>
 *   -----                      → <hr>
 *   blank line                 → paragraph break
 *   inline: =verb=, ~code~, *bold*, /italic/, _under_, [[link][label]]
 */
function renderOrgText(value) {
  const raw = String(value || "").trim();
  if (!raw) return `<span class="muted">No body recorded.</span>`;

  const lines  = raw.split("\n");
  const chunks = [];   // array of HTML strings
  let i = 0;
  let inPara  = false;
  let inUl    = false;
  let inOl    = false;

  function closePara() { if (inPara)  { chunks.push("</p>");  inPara  = false; } }
  function closeUl()   { if (inUl)    { chunks.push("</ul>"); inUl    = false; } }
  function closeOl()   { if (inOl)    { chunks.push("</ol>"); inOl    = false; } }
  function closeLists(){ closeUl(); closeOl(); }
  function closeAll()  { closePara(); closeLists(); }

  // ── inline markup (applied to already-escaped text won't work cleanly,
  //    so we escape *after* we detect block structure, inline last) ──
  function inlineMarkup(text) {
    return escHtml(text)
      // links first (before other replacements eat brackets)
      .replace(/\[\[([^\]]+)\]\[([^\]]+)\]\]/g, (_, _href, label) => `<a class="org-link">${escHtml(label)}</a>`)
      .replace(/\[\[([^\]]+)\]\]/g,              (_, href)         => `<a class="org-link">${escHtml(href)}</a>`)
      .replace(/=([^=\n]+?)=/g,   (_, t) => `<code>${escHtml(t)}</code>`)
      .replace(/~([^~\n]+?)~/g,   (_, t) => `<code>${escHtml(t)}</code>`)
      .replace(/\*([^*\n]+?)\*/g, (_, t) => `<strong>${escHtml(t)}</strong>`)
      .replace(/\/([^/\n]+?)\//g, (_, t) => `<em>${escHtml(t)}</em>`)
      .replace(/_([^_\n]+?)_/g,   (_, t) => `<span class="org-underline">${escHtml(t)}</span>`);
  }

  while (i < lines.length) {
    const line = lines[i];

    // ── skip property drawers ──
    if (/^:PROPERTIES:/i.test(line)) {
      while (i < lines.length && !/^:END:/i.test(lines[i])) i++;
      i++; continue;
    }

    // ── skip #+KEYWORD lines (metadata, not content) ──
    if (/^#\+(?:TITLE|AUTHOR|DATE|OPTIONS|FILETAGS|STARTUP|PROPERTY):/i.test(line)) {
      i++; continue;
    }

    // ── block structures ──
    const srcMatch = /^#\+BEGIN_(SRC|EXAMPLE|QUOTE)\b(.*)?/i.exec(line);
    if (srcMatch) {
      const kind  = srcMatch[1].toUpperCase();
      const lang  = (srcMatch[2] || "").trim();
      const block = [];
      i++;
      while (i < lines.length && !/^#\+END_/i.test(lines[i])) { block.push(lines[i]); i++; }
      i++;
      closeAll();
      const content = block.join("\n");
      if (kind === "QUOTE") {
        chunks.push(`<blockquote class="org-quote">${inlineMarkup(content)}</blockquote>`);
      } else {
        const langClass = lang ? ` class="lang-${escHtml(lang.split(" ")[0])}"` : "";
        chunks.push(`<pre class="org-src"><code${langClass}>${escHtml(content)}</code></pre>`);
      }
      continue;
    }

    // ── headings ──
    const headMatch = /^(\*{1,3})\s+(.+)/.exec(line);
    if (headMatch) {
      closeAll();
      const depth = headMatch[1].length; // 1 2 3
      const tag   = `h${depth + 1}`;     // h2 h3 h4
      chunks.push(`<${tag} class="org-h org-h${depth}">${inlineMarkup(headMatch[2])}</${tag}>`);
      i++; continue;
    }

    // ── horizontal rule ──
    if (/^-{4,}$/.test(line.trim())) {
      closeAll();
      chunks.push("<hr class=\"org-hr\">");
      i++; continue;
    }

    // ── unordered list ──
    const ulMatch = /^[ \t]*[-+]\s+(.+)/.exec(line);
    if (ulMatch) {
      closePara();
      closeOl();
      if (!inUl) { chunks.push("<ul class=\"org-list\">"); inUl = true; }
      chunks.push(`<li>${inlineMarkup(ulMatch[1])}</li>`);
      i++; continue;
    }

    // ── ordered list ──
    const olMatch = /^[ \t]*\d+[.)]\s+(.+)/.exec(line);
    if (olMatch) {
      closePara();
      closeUl();
      if (!inOl) { chunks.push("<ol class=\"org-list\">"); inOl = true; }
      chunks.push(`<li>${inlineMarkup(olMatch[1])}</li>`);
      i++; continue;
    }

    // ── blank line → paragraph break ──
    if (line.trim() === "") {
      closePara();
      closeLists();
      i++; continue;
    }

    // ── regular text → paragraph ──
    closeLists();
    if (!inPara) { chunks.push("<p>"); inPara = true; }
    else chunks.push("<br>");
    chunks.push(inlineMarkup(line));
    i++;
  }

  closeAll();

  return `<div class="doc-body">${chunks.join("")}</div>`;
}

// ── Org pane: scroll-to-expand ────────────────────────────────────────────────
const ORG_MIN    = 0;
const ORG_MAX    = 420;
const ORG_SNAP   = 180;
let   orgTarget  = 0;
let   orgCurrent = 0;
let   orgAnimId  = null;

function writeOrgPane(content) {
  const inner = document.getElementById("sheet-org-inner");
  if (!inner) return;
  inner.innerHTML = `<div class="doc-body org-body">${renderOrgText(content)}</div>`;
  if (!content || content.trim() === "") {
    setOrgHeight(0);
    orgTarget  = 0;
    orgCurrent = 0;
  }
}

function setOrgHeight(h) {
  inspectorEl.style.setProperty("--org-h", Math.round(h) + "px");
}

function animateOrg() {
  const diff = orgTarget - orgCurrent;
  if (Math.abs(diff) < 0.5) {
    orgCurrent = orgTarget;
    setOrgHeight(orgCurrent);
    orgAnimId = null;
    return;
  }
  orgCurrent += diff * 0.22;
  setOrgHeight(orgCurrent);
  orgAnimId = requestAnimationFrame(animateOrg);
}

function nudgeOrg(delta) {
  orgTarget = Math.max(ORG_MIN, Math.min(ORG_MAX, orgTarget + delta));
  if (orgAnimId === null) orgAnimId = requestAnimationFrame(animateOrg);
}

inspectorEl.addEventListener("wheel", (e) => {
  const inner = document.getElementById("sheet-org-inner");
  if (!inner) return;
  const atTop    = inner.scrollTop === 0;
  const atBottom = inner.scrollTop + inner.clientHeight >= inner.scrollHeight - 2;
  const expanding = e.deltaY > 0 && orgCurrent < ORG_MAX;
  const shrinking = e.deltaY < 0 && orgCurrent > ORG_MIN && atTop;
  if (expanding || shrinking) {
    e.preventDefault();
    nudgeOrg(e.deltaY * 0.55);
    return;
  }
  if (orgCurrent >= ORG_SNAP && !atTop && !atBottom) {
    return;
  }
}, { passive: false });

function openFileButton(target) {
  if (!target || !target.file || target.file === "external") return "No repository file.";
  const line = Math.max(1, Number(target.line) || 1);
  return `<button class="open-file-button" data-open-file="${escAttr(target.file)}" data-open-line="${line}">Open in editor</button>`;
}

function bindOpenButtons() {
  detailEl.querySelectorAll("button[data-open-file]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      btn.disabled = true;
      const orig = btn.textContent;
      btn.textContent = "Opening…";
      try {
        const res = await fetch("/api/open", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ file: btn.dataset.openFile, line: Number(btn.dataset.openLine) || 1 }),
        });
        if (!res.ok) throw new Error(await res.text());
        btn.textContent = "Opened ✓";
      } catch {
        btn.textContent = "Failed ✗";
      } finally {
        setTimeout(() => { btn.disabled = false; btn.textContent = orig; }, 900);
      }
    });
  });
}

// ─── Pointer ──────────────────────────────────────────────────────────────────
function updatePointer(e) {
  pointer.x = e.clientX;
  pointer.y = e.clientY;
  const w   = screenToWorld(pointer.x, pointer.y);
  pointer.worldX = w.x;
  pointer.worldY = w.y;
}

// ─── Utilities ────────────────────────────────────────────────────────────────
function clamp(v, lo, hi)   { return Math.max(lo, Math.min(hi, v)); }
function trunc(v, max)      { const s = String(v); return s.length > max ? s.slice(0, max-3)+"..." : s; }
function statusLabel(node)  { return node.status === "done" ? "DONE" : node.status === "blocked" ? "BLOCKED" : "TODO"; }

function escAttr(v) { return escHtml(v).replaceAll('"', "&quot;"); }
function escHtml(v) {
  return String(v).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#039;"})[c]);
}
