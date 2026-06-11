const canvas = document.querySelector("#graph");
const ctx = canvas.getContext("2d");
const statsEl = document.querySelector("#project-strip");
const searchEl = document.querySelector("#search");
const candidateEl = document.querySelector("#candidate-list");
const refreshEl = document.querySelector("#refresh");
const todoFilterEl = document.querySelector("#todo-filter");
const todoListEl = document.querySelector("#todo-list");
const kindFilterEl = document.querySelector("#kind-filter");
const methodSearchEl = document.querySelector("#method-search");
const methodTabsEl = document.querySelector("#method-tabs");
const methodListEl = document.querySelector("#method-list");
const graphStatusEl = document.querySelector("#graph-status");
const inspectorEl = document.querySelector("#inspector");
const titleEl = document.querySelector("#detail-title");
const overlineEl = document.querySelector("#detail-overline");
const kindEl = document.querySelector("#detail-kind");
const detailEl = document.querySelector("#detail-list");
const neighborEl = document.querySelector("#neighbor-list");

const palette = {
  file: "#8ab4f8",
  metadata: "#aab8b2",
  context: "#eef5f1",
  component: "#52d6ff",
  observation: "#52d6ff",
  inference: "#d0bcff",
  decision: "#ffb86b",
  todo: "#ffd400",
  done: "#49d28f",
  test: "#49d28f",
  "test-meta": "#49d28f",
  source: "#f4a261",
  method: "#ffffff",
  type: "#eef5f1",
  dependency: "#52d6ff",
  dependent: "#d0bcff",
};

let graph = { nodes: [], edges: [], stats: {} };
let nodeById = new Map();
let selected = null;
let selectedEdge = null;
let cascade = { nodes: new Set(), edges: new Set(), upstream: new Set(), downstream: new Set() };
let activeKind = "all";
let candidates = [];
let candidateIndex = 0;
let activeMethodGroup = "core";
let viewMode = "context";
let methodTree = null;
let methodByName = new Map();
let pointer = { x: 0, y: 0, worldX: 0, worldY: 0 };
let dragging = false;
let dragStart = null;
let camera = { x: 0, y: 0, zoom: 0.82, tx: 0, ty: 0, targetZoom: 0.82, zoomAnchor: null };
let lastFrame = performance.now();
let pendingDoneAnimationId = "";
let frameRequested = false;
let renderCache = { graph: null, nodes: [], edges: [] };
let doneAnimationUntil = 0;
let perfProbe = null;
let graphLoaded = false;

loadGraph();
scheduleFrame();
window.__visualizerPerf = runPerfProbe;

refreshEl.addEventListener("click", loadGraph);
todoFilterEl.addEventListener("change", () => {
  renderTodos();
  scheduleFrame();
});
methodSearchEl.addEventListener("input", () => {
  renderMethods();
  scheduleFrame();
});

methodTabsEl.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-group]");
  if (!button) return;
  activeMethodGroup = button.dataset.group;
  for (const item of methodTabsEl.querySelectorAll("button")) item.classList.toggle("active", item === button);
  renderMethods();
  scheduleFrame();
});

searchEl.addEventListener("input", () => {
  applyVisibility();
  updateCandidates();
  renderTodos();
  scheduleFrame();
});

searchEl.addEventListener("keydown", (event) => {
  if (event.key === "ArrowDown") {
    event.preventDefault();
    moveCandidate(1);
  } else if (event.key === "ArrowUp") {
    event.preventDefault();
    moveCandidate(-1);
  } else if (event.key === "Enter" && candidates[candidateIndex]) {
    event.preventDefault();
    selectNode(candidates[candidateIndex], true);
  } else if (event.key === "Escape") {
    candidateEl.classList.remove("open");
  }
});

searchEl.addEventListener("focus", () => updateCandidates());

kindFilterEl.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-kind]");
  if (!button) return;
  activeKind = button.dataset.kind;
  for (const item of kindFilterEl.querySelectorAll("button")) item.classList.toggle("active", item === button);
  applyVisibility();
  updateCandidates();
  scheduleFrame();
});

canvas.addEventListener("pointerdown", (event) => {
  canvas.setPointerCapture(event.pointerId);
  dragging = true;
  canvas.classList.add("dragging");
  dragStart = { x: event.clientX, y: event.clientY, cx: camera.tx, cy: camera.ty };
  scheduleFrame();
});

canvas.addEventListener("pointermove", (event) => {
  updatePointer(event);
  if (!dragging || !dragStart) return;
  const dx = (event.clientX - dragStart.x) / camera.zoom;
  const dy = (event.clientY - dragStart.y) / camera.zoom;
  camera.tx = dragStart.cx - dx;
  camera.ty = dragStart.cy - dy;
  camera.x = camera.tx;
  camera.y = camera.ty;
  camera.zoomAnchor = null;
  scheduleFrame();
});

canvas.addEventListener("pointerup", (event) => {
  updatePointer(event);
  canvas.releasePointerCapture(event.pointerId);
  canvas.classList.remove("dragging");
  const moved = dragStart && Math.hypot(event.clientX - dragStart.x, event.clientY - dragStart.y) > 5;
  dragging = false;
  dragStart = null;
  if (!moved) {
    const hit = nearestNode(pointer.worldX, pointer.worldY);
    if (hit) {
      selectActiveNode(hit, true);
      return;
    }
    const edge = nearestEdge(pointer.worldX, pointer.worldY);
    if (edge) {
      selectEdge(edge);
      return;
    }
    deselectAll();
  }
});

canvas.addEventListener("wheel", (event) => {
  event.preventDefault();
  updatePointer(event);
  const factor = Math.exp(-event.deltaY * 0.0012);
  camera.targetZoom = clamp(camera.targetZoom * factor, 0.28, 3.2);
  camera.zoomAnchor = {
    screenX: pointer.x,
    screenY: pointer.y,
    worldX: pointer.worldX,
    worldY: pointer.worldY,
  };
  scheduleFrame();
}, { passive: false });

async function loadGraph() {
  graphStatusEl.textContent = "syncing";
  const response = await fetch("/api/context");
  const payload = await response.json();
  graph = prepareGraph(payload);
  nodeById = new Map(graph.nodes.map((node) => [node.id, node]));
  methodByName = new Map((graph.methods || []).map((method) => [method.name, method]));
  applyVisibility();
  renderStats();
  renderTodos();
  renderMethods();
  updateCandidates();
  selectNode(selected ? nodeById.get(selected.id) || graph.nodes[0] : graph.nodes.find((node) => node.kind === "todo") || graph.nodes[0], false);
  graphStatusEl.textContent = `${graph.stats.objects} objects · ${graph.stats.openTodos} TODO`;
  graphLoaded = true;
  if (new URLSearchParams(window.location.search).has("perf")) runBrowserPerfReport();
}

function prepareGraph(payload) {
  const nodes = payload.nodes.map((node, index) => {
    const group = groupIndex(node.kind);
    const angle = index * 2.399963 + group * 0.51;
    const radius = 160 + group * 116 + (index % 17) * 4;
    const donePulseStarted = pendingDoneAnimationId === node.id && node.status === "done" ? performance.now() : 0;
    if (donePulseStarted) doneAnimationUntil = Math.max(doneAnimationUntil, donePulseStarted + 900);
    return {
      ...node,
      x: Math.cos(angle) * radius,
      y: Math.sin(angle) * radius,
      vx: 0,
      vy: 0,
      fx: Math.cos(angle) * radius,
      fy: Math.sin(angle) * radius,
      radius: node.kind === "todo" ? 15 : node.kind === "test" ? 8 : 10,
      visible: true,
      pulse: (index * 0.79) % (Math.PI * 2),
      donePulseStarted,
    };
  });
  pendingDoneAnimationId = "";
  const edges = payload.edges.map((edge, index) => ({ ...edge, id: `${edge.source}->${edge.target}:${edge.kind}:${index}` }));
  invalidateRenderCache();
  return { ...payload, nodes, edges };
}

function groupIndex(kind) {
  return { todo: 0, test: 1, decision: 2, observation: 3, component: 4, context: 5, file: 6, source: 7, metadata: 8 }[kind] ?? 5;
}

function applyVisibility() {
  viewMode = "context";
  methodTree = null;
  selectedEdge = null;
  cascade = emptyCascade();
  const q = searchEl.value.trim();
  for (const node of graph.nodes) {
    const kindMatch = kindMatches(node);
    const searchMatch = !q || fuzzyScore(node, q) > -Infinity;
    node.visible = kindMatch && searchMatch;
  }
  invalidateRenderCache();
}

function updateCandidates() {
  const q = searchEl.value.trim();
  const base = graph.nodes.filter((node) => kindMatches(node));
  candidates = (q ? base.map((node) => ({ node, score: fuzzyScore(node, q) })).filter((item) => item.score > -Infinity).sort((a, b) => b.score - a.score).map((item) => item.node) : base)
    .slice(0, 12);
  candidateIndex = Math.min(candidateIndex, Math.max(0, candidates.length - 1));
  renderCandidates();
  if (candidates[candidateIndex]) focusCandidate(candidates[candidateIndex]);
}

function kindMatches(node) {
  if (activeKind === "all") return true;
  if (activeKind === "todo") return node.kind === "todo" && node.status !== "done";
  if (activeKind === "done") return node.kind === "todo" && node.status === "done";
  if (activeKind === "context") return !["todo", "test", "decision"].includes(node.kind);
  return node.kind === activeKind;
}

function renderCandidates() {
  candidateEl.classList.toggle("open", searchEl.matches(":focus") && candidates.length > 0);
  candidateEl.innerHTML = candidates.map((node, index) => `
    <div class="candidate ${node.kind === "todo" ? "todo" : ""} ${index === candidateIndex ? "active" : ""}" data-index="${index}">
      <div class="candidate-kind">${escapeHtml(node.kind === "todo" ? "TODO" : node.kind)}</div>
      <div>
        <div class="candidate-title">${escapeHtml(node.label)}</div>
        <div class="candidate-file">${escapeHtml(node.file)}</div>
      </div>
    </div>
  `).join("");
  candidateEl.querySelectorAll(".candidate").forEach((item) => {
    item.addEventListener("mouseenter", () => {
      candidateIndex = Number(item.dataset.index);
      focusCandidate(candidates[candidateIndex]);
      renderCandidates();
    });
    item.addEventListener("click", () => selectNode(candidates[Number(item.dataset.index)], true));
  });
}

function moveCandidate(delta) {
  if (!candidates.length) return;
  candidateIndex = (candidateIndex + delta + candidates.length) % candidates.length;
  focusCandidate(candidates[candidateIndex]);
  renderCandidates();
}

function focusCandidate(node) {
  if (!node) return;
  viewMode = "context";
  methodTree = null;
  selectedEdge = null;
  selected = node;
  cascade = computeCascade([node.id]);
  camera.tx = node.x;
  camera.ty = node.y;
  camera.targetZoom = 1.65;
  updateInspector(node);
}

function fuzzyScore(node, query) {
  const haystack = `${node.kind} ${node.label} ${node.id} ${node.file} ${node.summary}`.toLowerCase();
  const needle = query.toLowerCase();
  let h = 0;
  let score = node.kind === "todo" ? 24 : 0;
  for (let n = 0; n < needle.length; n += 1) {
    const found = haystack.indexOf(needle[n], h);
    if (found === -1) return -Infinity;
    score += found === h ? 8 : Math.max(1, 8 - (found - h));
    h = found + 1;
  }
  if (haystack.includes(needle)) score += 32;
  return score;
}

function renderStats() {
  const cards = [
    ["Objects", graph.stats.objects, ""],
    ["Morphisms", graph.stats.morphisms, ""],
    ["TODO", graph.stats.openTodos, "todo-card"],
    ["DONE", graph.nodes.filter((node) => node.kind === "todo" && node.status === "done").length, "done-card"],
  ];
  statsEl.innerHTML = cards.map(([label, value, cls]) => `<article class="project-card ${cls}"><strong>${value}</strong><span>${label}</span></article>`).join("");
}

function renderTodos() {
  const filter = todoFilterEl.value;
  const todos = graph.nodes
    .filter((node) => node.kind === "todo")
    .filter((node) => filter === "all" || (filter === "done" ? node.status === "done" : node.status !== "done"))
    .sort((a, b) => Number(a.status === "done") - Number(b.status === "done") || a.label.localeCompare(b.label));

  todoListEl.innerHTML = todos.map((todo) => `
    <article class="todo-item ${todo.status === "done" ? "done" : ""}" data-id="${escapeAttr(todo.id)}">
      <p class="todo-title">${escapeHtml(todo.summary || todo.label || "TODO")}</p>
      <div class="todo-meta">${escapeHtml(todo.file)} · ${escapeHtml(todo.status || "open")}${todo.completed_at ? ` · ${escapeHtml(todo.completed_at)}` : ""}</div>
      <div class="todo-actions">
        <button data-status="open" class="state-open ${todo.status !== "done" && todo.status !== "blocked" ? "active" : ""}">Open</button>
        <button data-status="done" class="state-done ${todo.status === "done" ? "active" : ""}">Done</button>
        <button data-status="blocked" class="state-blocked ${todo.status === "blocked" ? "active" : ""}">Blocked</button>
      </div>
    </article>
  `).join("");

  todoListEl.querySelectorAll(".todo-item").forEach((item) => {
    item.addEventListener("click", (event) => {
      const node = nodeById.get(item.dataset.id);
      if (node) selectNode(node, true);
      const button = event.target.closest("button[data-status]");
      if (button) updateTodo(item.dataset.id, button.dataset.status);
    });
  });
}

function renderMethods() {
  const q = methodSearchEl.value.trim();
  const methods = (graph.methods || [])
    .filter((method) => method.group === activeMethodGroup)
    .map((method) => ({ method, score: methodScore(method, q) }))
    .filter((item) => item.score > -Infinity)
    .sort((a, b) => b.score - a.score || a.method.name.localeCompare(b.method.name))
    .slice(0, 80)
    .map((item) => item.method);

  methodListEl.innerHTML = methods.map((method, index) => `
    <article class="method-item" data-index="${index}">
      <strong>${escapeHtml(method.name)}</strong>
      <div class="method-signature">${escapeHtml(method.signature || "value")}</div>
      <div class="method-file">${escapeHtml(method.file)}:${method.line}</div>
    </article>
  `).join("");

  methodListEl.querySelectorAll(".method-item").forEach((item) => {
    item.addEventListener("click", () => showMethod(methods[Number(item.dataset.index)]));
  });
}

function methodScore(method, query) {
  if (!query) return 1;
  const name = method.name.toLowerCase();
  const signature = method.signature.toLowerCase();
  const file = method.file.toLowerCase();
  const q = query.toLowerCase();
  let score = 0;
  if (name === q) score += 220;
  if (name.includes(q)) score += 90;
  if (signature.includes(q)) score += 82;
  const best = Math.max(
    fuzzyTextScore(name, q) + 46,
    fuzzyTextScore(signature, q) + 26,
    fuzzyTextScore(file, q)
  );
  if (best === -Infinity && score === 0) return -Infinity;
  return score + Math.max(0, best);
}

function fuzzyTextScore(haystack, needle) {
  let h = 0;
  let score = 0;
  for (let n = 0; n < needle.length; n += 1) {
    const found = haystack.indexOf(needle[n], h);
    if (found === -1) return -Infinity;
    score += found === h ? 8 : Math.max(1, 8 - (found - h));
    h = found + 1;
  }
  return score;
}

function showMethod(method) {
  if (!method) return;
  methodTree = buildMethodTree(method);
  invalidateRenderCache();
  viewMode = "method";
  selected = null;
  selectedEdge = methodTree.edges[0] || null;
  cascade = selectedEdge ? computeCascade([selectedEdge.source, selectedEdge.target], selectedEdge.id) : emptyCascade();
  camera.tx = 0;
  camera.ty = 0;
  camera.targetZoom = 1.05;
  camera.zoomAnchor = null;
  inspectorEl.classList.remove("todo-selected");
  overlineEl.textContent = method.group === "prelude" ? "Prelude Method" : "Core Method";
  titleEl.textContent = method.name;
  kindEl.textContent = `${method.group} · ${method.form}`;
  detailEl.innerHTML = [
    ["Signature", method.signature || "value"],
    ["File", `${method.file}:${method.line}`],
    ["Uses", method.uses.length ? method.uses.join(", ") : "No parsed core calls."],
    ["Used by", method.used_by.length ? method.used_by.join(", ") : "No parsed callers."],
    ["Open", openFileButton(method), true],
  ].map(renderDetailRow).join("");
  bindOpenButtons(method);
  neighborEl.innerHTML = methodTree.edges.slice(1, 11).map((edge) =>
    `<div class="neighbor" data-id="${escapeAttr(edge.id)}"><strong>${escapeHtml(edge.label)}</strong><span>${escapeHtml(edge.kind)} · ${escapeHtml(edge.method?.file || "")}</span></div>`
  ).join("");
  neighborEl.querySelectorAll(".neighbor").forEach((item) => {
    item.addEventListener("click", () => {
      const edge = methodTree.edges.find((candidate) => candidate.id === item.dataset.id);
      if (edge) selectEdge(edge);
    });
  });
}

function buildMethodTree(method) {
  const nodes = [];
  const edges = [];
  addMethodArrow(method, "method", 0, 0, nodes, edges);
  addMethodRing(method.uses || [], "dependency", -1, method, nodes, edges);
  addMethodRing(method.used_by || [], "dependent", 1, method, nodes, edges);
  return { nodes, edges };
}

function addMethodRing(names, kind, side, root, nodes, edges) {
  const count = Math.max(1, names.length);
  names.forEach((name, index) => {
    const source = methodByName.get(name) || { id: `method:external:${name}`, name, signature: "", group: "external", file: "external" };
    const spread = (index - (count - 1) / 2) * 72;
    const x = side * 310;
    const y = spread;
    addMethodArrow(source, kind, x, y, nodes, edges);
  });
}

function addMethodArrow(method, kind, x, y, nodes, edges) {
  const parts = signatureParts(method.signature || "");
  const source = makeTypeNode(`${method.id}:source`, parts.source, method.file, x - 76, y, nodes.length);
  const target = makeTypeNode(`${method.id}:target`, parts.target, method.file, x + 76, y, nodes.length + 1);
  nodes.push(source, target);
  edges.push({
    id: `${kind}:${method.id}`,
    source: source.id,
    target: target.id,
    label: method.name,
    kind: kind === "method" ? "function" : kind,
    method,
  });
}

function makeTypeNode(id, label, file, x, y, index) {
  return {
    id,
    label,
    kind: "type",
    file,
    summary: "type object",
    x,
    y,
    fx: x,
    fy: y,
    vx: 0,
    vy: 0,
    radius: 13,
    visible: true,
    pulse: index * 0.8,
  };
}

function signatureParts(signature) {
  const parts = String(signature || "value").split("->").map((part) => part.trim()).filter(Boolean);
  if (parts.length < 2) return { source: "Unit", target: parts[0] || "Value" };
  return {
    source: parts.slice(0, -1).join(" -> "),
    target: parts[parts.length - 1],
  };
}

async function updateTodo(id, status) {
  graphStatusEl.textContent = "writing TODO";
  pendingDoneAnimationId = status === "done" ? id : "";
  await fetch("/api/todo", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ id, status }),
  });
  await loadGraph();
}

function frame(now) {
  frameRequested = false;
  const dt = Math.min(0.04, (now - lastFrame) / 1000);
  lastFrame = now;
  const frameStart = performance.now();
  resizeCanvas();
  simulate(dt, now / 1000);
  const cameraMoving = updateCamera(dt);
  draw(now / 1000);
  if (perfProbe) recordPerfFrame(performance.now() - frameStart);
  if (cameraMoving || dragging || now < doneAnimationUntil) scheduleFrame();
}

function scheduleFrame() {
  if (frameRequested) return;
  frameRequested = true;
  requestAnimationFrame(frame);
}

function runPerfProbe(frames = 180) {
  perfProbe = { frames: [], remaining: frames, resolve: null };
  const promise = new Promise((resolve) => {
    perfProbe.resolve = resolve;
  });
  scheduleFrame();
  return promise;
}

function recordPerfFrame(duration) {
  perfProbe.frames.push(duration);
  perfProbe.remaining -= 1;
  if (perfProbe.remaining > 0) {
    scheduleFrame();
    return;
  }
  const sorted = [...perfProbe.frames].sort((a, b) => a - b);
  const result = {
    frames: sorted.length,
    avg: sorted.reduce((sum, value) => sum + value, 0) / sorted.length,
    p95: sorted[Math.floor(sorted.length * 0.95)],
    max: sorted[sorted.length - 1],
  };
  perfProbe.resolve(result);
  perfProbe = null;
}

async function runBrowserPerfReport() {
  await new Promise((resolve) => setTimeout(resolve, 120));
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

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const width = Math.floor(window.innerWidth * dpr);
  const height = Math.floor(window.innerHeight * dpr);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
}

function simulate(dt, time) {
  return;
}

function updateCamera(dt) {
  const beforeX = camera.x;
  const beforeY = camera.y;
  const beforeZoom = camera.zoom;
  const cameraEase = 1 - Math.exp(-dt * 26);
  const zoomEase = 1 - Math.exp(-dt * 30);
  camera.zoom += (camera.targetZoom - camera.zoom) * zoomEase;
  if (camera.zoomAnchor) {
    camera.tx = camera.zoomAnchor.worldX - (camera.zoomAnchor.screenX - window.innerWidth / 2) / camera.targetZoom;
    camera.ty = camera.zoomAnchor.worldY - (camera.zoomAnchor.screenY - window.innerHeight / 2) / camera.targetZoom;
    if (Math.abs(camera.zoom - camera.targetZoom) < 0.002) camera.zoomAnchor = null;
  }
  camera.x += (camera.tx - camera.x) * cameraEase;
  camera.y += (camera.ty - camera.y) * cameraEase;
  return Math.abs(camera.x - beforeX) > 0.01
    || Math.abs(camera.y - beforeY) > 0.01
    || Math.abs(camera.zoom - beforeZoom) > 0.0005
    || Math.abs(camera.x - camera.tx) > 0.01
    || Math.abs(camera.y - camera.ty) > 0.01
    || Math.abs(camera.zoom - camera.targetZoom) > 0.0005;
}

function draw(time) {
  const dpr = window.devicePixelRatio || 1;
  const width = canvas.width / dpr;
  const height = canvas.height / dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const bg = ctx.createRadialGradient(width / 2, height / 2, 20, width / 2, height / 2, Math.max(width, height));
  bg.addColorStop(0, "#162024");
  bg.addColorStop(1, "#0d1113");
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, width, height);
  drawGrid(width, height);

  ctx.save();
  ctx.translate(width / 2, height / 2);
  ctx.scale(camera.zoom, camera.zoom);
  ctx.translate(-camera.x, -camera.y);
  const cache = getRenderCache();
  const bounds = visibleWorldBounds(width, height, 80);
  for (const item of cache.edges) {
    if (!segmentIntersectsBounds(item.source, item.target, bounds)) continue;
    drawEdge(item.source, item.target, item.edge, time);
  }
  for (const node of cache.nodes) {
    if (!pointNearBounds(node, bounds)) continue;
    drawNode(node, time);
  }
  ctx.restore();
}

function visibleWorldBounds(width, height, margin) {
  const topLeft = screenToWorld(-margin, -margin);
  const bottomRight = screenToWorld(width + margin, height + margin);
  return { minX: topLeft.x, minY: topLeft.y, maxX: bottomRight.x, maxY: bottomRight.y };
}

function pointNearBounds(node, bounds) {
  return node.x >= bounds.minX && node.x <= bounds.maxX && node.y >= bounds.minY && node.y <= bounds.maxY;
}

function segmentIntersectsBounds(a, b, bounds) {
  return Math.max(a.x, b.x) >= bounds.minX
    && Math.min(a.x, b.x) <= bounds.maxX
    && Math.max(a.y, b.y) >= bounds.minY
    && Math.min(a.y, b.y) <= bounds.maxY;
}

function activeGraph() {
  return viewMode === "method" && methodTree ? methodTree : graph;
}

function findActiveNode(active, id) {
  return active === graph ? nodeById.get(id) : active.nodes.find((node) => node.id === id);
}

function invalidateRenderCache() {
  renderCache.graph = null;
}

function getRenderCache() {
  const active = activeGraph();
  if (renderCache.graph === active) return renderCache;
  const nodes = active.nodes.filter((node) => node.visible);
  const activeNodeById = active === graph ? nodeById : new Map(active.nodes.map((node) => [node.id, node]));
  const edges = active.edges
    .map((edge) => ({ edge, source: activeNodeById.get(edge.source), target: activeNodeById.get(edge.target) }))
    .filter((item) => item.source && item.target && item.source.visible && item.target.visible);
  renderCache = { graph: active, nodes, edges };
  return renderCache;
}

function emptyCascade() {
  return { nodes: new Set(), edges: new Set(), upstream: new Set(), downstream: new Set() };
}

function computeCascade(seedNodeIds, seedEdgeId = "") {
  const active = activeGraph();
  const outgoing = new Map();
  const incoming = new Map();
  for (const edge of active.edges) {
    if (!outgoing.has(edge.source)) outgoing.set(edge.source, []);
    if (!incoming.has(edge.target)) incoming.set(edge.target, []);
    outgoing.get(edge.source).push(edge);
    incoming.get(edge.target).push(edge);
  }
  const downstream = walkCascade(seedNodeIds, outgoing, "target");
  const upstream = walkCascade(seedNodeIds, incoming, "source");
  const nodes = new Set([...seedNodeIds, ...downstream.nodes, ...upstream.nodes]);
  const edges = new Set([...downstream.edges, ...upstream.edges]);
  if (seedEdgeId) edges.add(seedEdgeId);
  return { nodes, edges, upstream: upstream.nodes, downstream: downstream.nodes };
}

function walkCascade(seedNodeIds, adjacency, nextKey) {
  const nodes = new Set();
  const edges = new Set();
  const queue = [...seedNodeIds];
  const seen = new Set(seedNodeIds);
  while (queue.length && seen.size < 512) {
    const id = queue.shift();
    for (const edge of adjacency.get(id) || []) {
      edges.add(edge.id);
      const next = edge[nextKey];
      if (seen.has(next)) continue;
      seen.add(next);
      nodes.add(next);
      queue.push(next);
    }
  }
  return { nodes, edges };
}

function drawGrid(width, height) {
  ctx.save();
  const topLeft = screenToWorld(0, 0);
  const bottomRight = screenToWorld(width, height);
  const minorStep = niceGridStep(64 / camera.zoom);
  const majorStep = minorStep * 5;
  drawGridLines(topLeft.x, bottomRight.x, topLeft.y, bottomRight.y, minorStep, width, height, "rgba(238,245,241,0.045)", 1);
  drawGridLines(topLeft.x, bottomRight.x, topLeft.y, bottomRight.y, majorStep, width, height, "rgba(238,245,241,0.11)", 1.2);
  drawAxis(width, height);
  ctx.restore();
}

function drawGridLines(minX, maxX, minY, maxY, step, width, height, color, lineWidth) {
  ctx.strokeStyle = color;
  ctx.lineWidth = lineWidth;
  const startX = Math.floor(minX / step) * step;
  const endX = Math.ceil(maxX / step) * step;
  for (let x = startX; x <= endX; x += step) {
    const sx = worldToScreenX(x, width);
    crispLine(sx, 0, sx, height);
  }
  const startY = Math.floor(minY / step) * step;
  const endY = Math.ceil(maxY / step) * step;
  for (let y = startY; y <= endY; y += step) {
    const sy = worldToScreenY(y, height);
    crispLine(0, sy, width, sy);
  }
}

function drawAxis(width, height) {
  ctx.lineWidth = 1.4;
  ctx.strokeStyle = "rgba(82,214,255,0.22)";
  const x = worldToScreenX(0, width);
  if (x >= 0 && x <= width) crispLine(x, 0, x, height);
  ctx.strokeStyle = "rgba(73,210,143,0.22)";
  const y = worldToScreenY(0, height);
  if (y >= 0 && y <= height) crispLine(0, y, width, y);
}

function crispLine(x1, y1, x2, y2) {
  ctx.beginPath();
  ctx.moveTo(Math.round(x1) + 0.5, Math.round(y1) + 0.5);
  ctx.lineTo(Math.round(x2) + 0.5, Math.round(y2) + 0.5);
  ctx.stroke();
}

function niceGridStep(targetWorldSize) {
  const power = Math.pow(10, Math.floor(Math.log10(targetWorldSize)));
  const normalized = targetWorldSize / power;
  const factor = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
  return factor * power;
}

function drawEdge(a, b, edge, time) {
  const edgeSelected = selectedEdge && selectedEdge.id === edge.id;
  const connectedToSelection = selected && (edge.source === selected.id || edge.target === selected.id);
  const inCascade = cascade.edges.has(edge.id);
  const alpha = edgeSelected ? 0.92 : connectedToSelection ? 0.74 : inCascade ? 0.48 : 0.10;
  const mx = (a.x + b.x) / 2;
  const my = (a.y + b.y) / 2;
  const cx = mx - (b.y - a.y) * 0.08;
  const cy = my + (b.x - a.x) * 0.08;
  const end = trimEndpoint(cx, cy, b.x, b.y, b.radius + 4);
  ctx.beginPath();
  ctx.moveTo(a.x, a.y);
  ctx.quadraticCurveTo(cx, cy, end.x, end.y);
  ctx.strokeStyle = edgeColor(edge, alpha);
  ctx.lineWidth = edgeSelected ? 4 : connectedToSelection ? 2.6 : 1.1;
  ctx.stroke();
  drawArrowhead(cx, cy, end.x, end.y, ctx.strokeStyle, edgeSelected || connectedToSelection ? 9 : 7);
  if (edgeSelected || edge.method || connectedToSelection) {
    ctx.save();
    ctx.fillStyle = edge.kind === "function" ? "#ffffff" : "#eef5f1";
    ctx.font = `${edgeSelected ? 800 : 650} 12px Inter, Roboto, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.shadowBlur = 16;
    ctx.shadowColor = "rgba(0,0,0,0.65)";
    ctx.fillText(truncate(edge.label, 28), cx, cy - 10);
    ctx.restore();
  }
}

function edgeColor(edge, alpha) {
  if (edge.kind === "verifies") return `rgba(73,210,143,${alpha})`;
  if (edge.kind === "evidence") return `rgba(255,212,0,${alpha})`;
  if (edge.kind === "function") return `rgba(255,255,255,${Math.max(alpha, 0.72)})`;
  if (edge.kind === "dependency") return `rgba(82,214,255,${Math.max(alpha, 0.46)})`;
  if (edge.kind === "dependent") return `rgba(208,188,255,${Math.max(alpha, 0.46)})`;
  if (edge.kind === "uses") return `rgba(82,214,255,${Math.max(alpha, 0.46)})`;
  if (edge.kind === "used-by") return `rgba(208,188,255,${Math.max(alpha, 0.46)})`;
  return `rgba(238,245,241,${alpha})`;
}

function trimEndpoint(fromX, fromY, toX, toY, amount) {
  const dx = toX - fromX;
  const dy = toY - fromY;
  const dist = Math.max(1, Math.hypot(dx, dy));
  return { x: toX - (dx / dist) * amount, y: toY - (dy / dist) * amount };
}

function drawArrowhead(fromX, fromY, toX, toY, color, size) {
  const angle = Math.atan2(toY - fromY, toX - fromX);
  ctx.save();
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.moveTo(toX, toY);
  ctx.lineTo(toX - Math.cos(angle - 0.48) * size, toY - Math.sin(angle - 0.48) * size);
  ctx.lineTo(toX - Math.cos(angle + 0.48) * size, toY - Math.sin(angle + 0.48) * size);
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

function drawNode(node, time) {
  const color = nodeColor(node);
  const isSelected = selected && selected.id === node.id;
  const inCascade = cascade.nodes.has(node.id);
  const r = node.radius + (isSelected ? 7 : 0);
  ctx.save();
  ctx.shadowBlur = isSelected ? 18 : 0;
  if (isSelected) ctx.shadowColor = color;
  ctx.beginPath();
  ctx.arc(node.x, node.y, Math.max(3, r), 0, Math.PI * 2);
  ctx.fillStyle = color;
  ctx.globalAlpha = isSelected ? 1 : inCascade ? 0.94 : node.kind === "todo" ? 0.86 : 0.54;
  ctx.fill();
  ctx.globalAlpha = 1;
  ctx.shadowBlur = 0;
  ctx.strokeStyle = isSelected ? "#ffffff" : inCascade ? "rgba(255,255,255,0.72)" : node.kind === "todo" ? "rgba(255,244,184,0.72)" : "rgba(255,255,255,0.30)";
  ctx.lineWidth = isSelected ? 3.4 : inCascade ? 2.1 : node.kind === "todo" ? 1.6 : 1;
  ctx.stroke();
  if (isSelected || node.kind === "todo") {
    ctx.font = `${node.kind === "todo" ? 700 : 500} 13px Inter, Roboto, sans-serif`;
    ctx.fillStyle = node.kind === "todo" ? "#ffe66d" : "#eef5f1";
    ctx.textAlign = "center";
    ctx.fillText(truncate(node.label, 30), node.x, node.y - r - 12);
  }
  ctx.restore();
}

function nodeColor(node) {
  if (node.kind !== "todo" || node.status !== "done") return palette[node.kind] || palette.context;
  if (!node.donePulseStarted) return palette.done;
  const t = clamp((performance.now() - node.donePulseStarted) / 850, 0, 1);
  return lerpHex(palette.todo, palette.done, easeOutCubic(t));
}

function lerpHex(a, b, t) {
  const av = hexToRgb(a);
  const bv = hexToRgb(b);
  return `rgb(${Math.round(av.r + (bv.r - av.r) * t)}, ${Math.round(av.g + (bv.g - av.g) * t)}, ${Math.round(av.b + (bv.b - av.b) * t)})`;
}

function hexToRgb(value) {
  const hex = value.replace("#", "");
  return {
    r: parseInt(hex.slice(0, 2), 16),
    g: parseInt(hex.slice(2, 4), 16),
    b: parseInt(hex.slice(4, 6), 16),
  };
}

function easeOutCubic(t) {
  return 1 - Math.pow(1 - t, 3);
}

function updatePointer(event) {
  pointer.x = event.clientX;
  pointer.y = event.clientY;
  const world = screenToWorld(pointer.x, pointer.y);
  pointer.worldX = world.x;
  pointer.worldY = world.y;
}

function screenToWorld(x, y) {
  return {
    x: (x - window.innerWidth / 2) / camera.zoom + camera.x,
    y: (y - window.innerHeight / 2) / camera.zoom + camera.y,
  };
}

function worldToScreenX(x, width = window.innerWidth) {
  return (x - camera.x) * camera.zoom + width / 2;
}

function worldToScreenY(y, height = window.innerHeight) {
  return (y - camera.y) * camera.zoom + height / 2;
}

function nearestNode(x, y) {
  let best = null;
  let bestDistance = Infinity;
  for (const node of getRenderCache().nodes) {
    const distance = Math.hypot(node.x - x, node.y - y);
    if (distance < node.radius + 16 && distance < bestDistance) {
      best = node;
      bestDistance = distance;
    }
  }
  return best;
}

function nearestEdge(x, y) {
  let best = null;
  let bestDistance = Infinity;
  for (const item of getRenderCache().edges) {
    const distance = distanceToSegment(x, y, item.source.x, item.source.y, item.target.x, item.target.y);
    if (distance < 16 / camera.zoom && distance < bestDistance) {
      best = item.edge;
      bestDistance = distance;
    }
  }
  return best;
}

function distanceToSegment(px, py, ax, ay, bx, by) {
  const dx = bx - ax;
  const dy = by - ay;
  const length2 = dx * dx + dy * dy || 1;
  const t = clamp(((px - ax) * dx + (py - ay) * dy) / length2, 0, 1);
  const x = ax + dx * t;
  const y = ay + dy * t;
  return Math.hypot(px - x, py - y);
}

function selectNode(node, animate = true) {
  if (!node) return;
  viewMode = "context";
  methodTree = null;
  selected = node;
  selectedEdge = null;
  cascade = computeCascade([node.id]);
  camera.tx = node.x;
  camera.ty = node.y;
  camera.targetZoom = animate ? 1.72 : 0.9;
  updateInspector(node);
  scheduleFrame();
}

function selectActiveNode(node, animate = true) {
  if (viewMode === "method") {
    selected = node;
    selectedEdge = null;
    cascade = computeCascade([node.id]);
    camera.tx = node.x;
    camera.ty = node.y;
    camera.targetZoom = animate ? 1.35 : 1.05;
    scheduleFrame();
    return;
  }
  selectNode(node, animate);
}

function selectEdge(edge) {
  if (!edge) return;
  selectedEdge = edge;
  selected = null;
  cascade = computeCascade([edge.source, edge.target], edge.id);
  updateEdgeInspector(edge);
  scheduleFrame();
}

function deselectAll() {
  selected = null;
  selectedEdge = null;
  cascade = emptyCascade();
  overlineEl.textContent = "No Selection";
  titleEl.textContent = "None";
  kindEl.textContent = "No selection";
  detailEl.innerHTML = "";
  neighborEl.innerHTML = "";
  scheduleFrame();
}

function updateInspector(node) {
  inspectorEl.classList.toggle("todo-selected", node.kind === "todo");
  overlineEl.textContent = node.kind === "todo" ? "TODO" : "Selected Object";
  titleEl.textContent = node.label;
  kindEl.textContent = `${node.kind === "todo" ? "TODO" : node.kind} · ${node.file}`;
  detailEl.innerHTML = [
    ["ID", node.id],
    ["Summary", node.summary || "No summary recorded."],
    ["Confidence", node.confidence || "not specified"],
    ["Status", node.status || "active"],
    ["Outgoing", graph.edges.filter((edge) => edge.source === node.id).length],
    ["Incoming", graph.edges.filter((edge) => edge.target === node.id).length],
    ["Cascade", `${cascade.nodes.size} objects · ${cascade.edges.size} morphisms`],
    ["Open", openFileButton(node), true],
  ].map(renderDetailRow).join("");
  bindOpenButtons(node);
  renderNeighbors(node);
}

function updateEdgeInspector(edge) {
  const active = activeGraph();
  const source = findActiveNode(active, edge.source);
  const target = findActiveNode(active, edge.target);
  inspectorEl.classList.remove("todo-selected");
  overlineEl.textContent = edge.method ? "Function Morphism" : "Selected Morphism";
  titleEl.textContent = edge.method ? edge.method.name : edge.label;
  kindEl.textContent = `${edge.kind} · ${source?.label || edge.source} -> ${target?.label || edge.target}`;
  detailEl.innerHTML = [
    ["Arrow", edge.label],
    ["Source", source ? `${source.label} · ${source.file}` : edge.source],
    ["Target", target ? `${target.label} · ${target.file}` : edge.target],
    ["Signature", edge.method?.signature || "typed relation"],
    ["Cascade", `${cascade.nodes.size} objects · ${cascade.edges.size} morphisms`],
    ["Open", openFileButton(edge.method || source || target), true],
  ].map(renderDetailRow).join("");
  bindOpenButtons(edge.method || source || target);
  neighborEl.innerHTML = [source, target].filter(Boolean).map((node) =>
    `<div class="neighbor" data-id="${escapeAttr(node.id)}"><strong>${escapeHtml(node.label)}</strong><span>${escapeHtml(node.kind)} · ${escapeHtml(node.file)}</span></div>`
  ).join("");
  neighborEl.querySelectorAll(".neighbor").forEach((item) => {
    item.addEventListener("click", () => {
      const node = findActiveNode(activeGraph(), item.dataset.id);
      if (node) selectActiveNode(node, true);
    });
  });
}

function renderNeighbors(node) {
  const cascadeNodes = [...cascade.nodes]
    .filter((id) => id !== node.id)
    .map((id) => findActiveNode(activeGraph(), id))
    .filter(Boolean)
    .slice(0, 8);
  const immediate = graph.edges
    .filter((edge) => edge.source === node.id || edge.target === node.id)
    .slice(0, Math.max(0, 10 - cascadeNodes.length))
    .map((edge) => {
      const otherId = edge.source === node.id ? edge.target : edge.source;
      const other = nodeById.get(otherId);
      if (!other) return "";
      return `<div class="neighbor" data-id="${escapeAttr(other.id)}"><strong>${escapeHtml(truncate(other.label, 44))}</strong><span>${escapeHtml(edge.label)} · ${escapeHtml(other.kind === "todo" ? "TODO" : other.kind)}</span></div>`;
    });
  neighborEl.innerHTML = [
    ...cascadeNodes.map((other) => `<div class="neighbor" data-id="${escapeAttr(other.id)}"><strong>${escapeHtml(truncate(other.label, 44))}</strong><span>cascade · ${escapeHtml(other.kind === "todo" ? statusLabel(other) : other.kind)}</span></div>`),
    ...immediate,
  ].join("");
  neighborEl.querySelectorAll(".neighbor").forEach((item) => {
    item.addEventListener("click", () => selectNode(nodeById.get(item.dataset.id), true));
  });
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function truncate(value, max) {
  return String(value).length > max ? `${String(value).slice(0, max - 3)}...` : String(value);
}

function renderDetailRow([key, value, html = false]) {
  return `<dt>${escapeHtml(key)}</dt><dd>${html ? value : escapeHtml(String(value))}</dd>`;
}

function openFileButton(target) {
  if (!target || !target.file || target.file === "external") return "No repository file.";
  const line = Math.max(1, Number(target.line) || 1);
  return `<button class="open-file-button" data-open-file="${escapeAttr(target.file)}" data-open-line="${line}">Open in editor</button>`;
}

function bindOpenButtons() {
  detailEl.querySelectorAll("button[data-open-file]").forEach((button) => {
    button.addEventListener("click", async () => {
      button.disabled = true;
      const original = button.textContent;
      button.textContent = "Opening";
      try {
        const response = await fetch("/api/open", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ file: button.dataset.openFile, line: Number(button.dataset.openLine) || 1 }),
        });
        if (!response.ok) throw new Error(await response.text());
        button.textContent = "Opened";
      } catch (_error) {
        button.textContent = "Failed";
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = original;
        }, 900);
      }
    });
  });
}

function statusLabel(node) {
  if (node.status === "done") return "DONE";
  if (node.status === "blocked") return "BLOCKED";
  return "TODO";
}

function escapeAttr(value) {
  return escapeHtml(value).replaceAll('"', "&quot;");
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#039;",
  })[char]);
}
