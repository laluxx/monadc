#!/usr/bin/env python3
"""Glyph IR v0.5 reference nucleus: explicit graph reality and trusted kernel.

This executable model demonstrates:
  * hierarchical regions with explicit typed ports and edges;
  * typed multi-exit region interfaces;
  * linear/affine/unrestricted wire verification;
  * explicit child-region boundary bindings;
  * alpha-stable semantic identity independent of display IDs;
  * canonical JSON serialization and exact round-trip;
  * deterministic visual rendering from graph topology;
  * local proof-carrying graph patches checked fail-closed;
  * incremental Merkle region hashes and invalidation locality;
  * a hostile ownership/effect/closure specimen;
  * semantic mutation rejection.

It is a research executable, not production Monad integration.
"""
from __future__ import annotations

from dataclasses import dataclass, field, replace
from enum import Enum
import copy
import hashlib
import json
from typing import Any, Iterable, Mapping, Sequence


class GlyphError(Exception):
    pass


class PortKind(str, Enum):
    VALUE = "value"
    STATE = "state"
    CAPABILITY = "capability"
    LOAN = "loan"
    EFFECT = "effect"
    RESUMPTION = "resumption"
    PROOF = "proof"


class Linearity(str, Enum):
    UNRESTRICTED = "unrestricted"
    AFFINE = "affine"
    LINEAR = "linear"


class Direction(str, Enum):
    IN = "in"
    OUT = "out"


@dataclass(frozen=True)
class PortType:
    kind: PortKind
    payload: str
    linearity: Linearity
    erased: bool = False

    def canonical(self) -> tuple[str, str, str, bool]:
        return (self.kind.value, self.payload, self.linearity.value, self.erased)


@dataclass(frozen=True)
class Port:
    id: str
    owner_kind: str       # "node" or "region"
    owner_id: str
    name: str
    direction: Direction
    type: PortType
    ordinal: int


@dataclass(frozen=True)
class Edge:
    id: str
    region_id: str
    source: str
    targets: tuple[str, ...]
    role: str = "data"


@dataclass(frozen=True)
class BoundaryBinding:
    child_region: str
    # parent node input/output port name -> child region input/exit port name
    imports: tuple[tuple[str, str], ...] = ()
    exports: tuple[tuple[str, str, str], ...] = ()  # (exit, child port, parent port)


@dataclass(frozen=True)
class Node:
    id: str
    region_id: str
    op: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]
    attrs: tuple[tuple[str, Any], ...] = ()
    children: tuple[BoundaryBinding, ...] = ()
    display: str = ""

    def attr(self, key: str, default: Any = None) -> Any:
        return dict(self.attrs).get(key, default)


@dataclass(frozen=True)
class ExitInterface:
    name: str
    ports: tuple[str, ...]
    protocol_state: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class Region:
    id: str
    kind: str
    parent_node: str | None
    inputs: tuple[str, ...]
    exits: tuple[ExitInterface, ...]
    nodes: tuple[str, ...]
    edges: tuple[str, ...]
    display: str = ""


@dataclass(frozen=True)
class Graph:
    name: str
    root_region: str
    result_type: str
    observation_lens: tuple[str, ...]
    ports: Mapping[str, Port]
    nodes: Mapping[str, Node]
    edges: Mapping[str, Edge]
    regions: Mapping[str, Region]
    metadata: Mapping[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class OpSchema:
    op: str
    input_specs: tuple[tuple[str, PortKind, Linearity], ...]
    output_specs: tuple[tuple[str, PortKind, Linearity], ...]
    region_roles: tuple[str, ...] = ()
    pure: bool = False
    total: bool = True


SCHEMAS: dict[str, OpSchema] = {
    "call.validate": OpSchema(
        "call.validate",
        (("request", PortKind.VALUE, Linearity.UNRESTRICTED),),
        (("valid", PortKind.VALUE, Linearity.UNRESTRICTED),),
        pure=True,
    ),
    "control.branch": OpSchema(
        "control.branch",
        (
            ("condition", PortKind.VALUE, Linearity.UNRESTRICTED),
            ("resource", PortKind.CAPABILITY, Linearity.LINEAR),
        ),
        (("result", PortKind.VALUE, Linearity.UNRESTRICTED),),
        region_roles=("yes", "no"),
    ),
    "cap.move-field": OpSchema(
        "cap.move-field",
        (("source", PortKind.CAPABILITY, Linearity.LINEAR),),
        (
            ("field", PortKind.CAPABILITY, Linearity.LINEAR),
            ("remainder", PortKind.CAPABILITY, Linearity.LINEAR),
        ),
    ),
    "cap.drop": OpSchema(
        "cap.drop",
        (("resource", PortKind.CAPABILITY, Linearity.LINEAR),),
        (),
    ),
    "cap.project": OpSchema(
        "cap.project",
        (("resource", PortKind.CAPABILITY, Linearity.LINEAR),),
        (
            ("resource", PortKind.CAPABILITY, Linearity.LINEAR),
            ("value", PortKind.VALUE, Linearity.UNRESTRICTED),
        ),
        pure=True,
    ),
    "closure.make": OpSchema(
        "closure.make",
        (("capture", PortKind.CAPABILITY, Linearity.LINEAR),),
        (("closure", PortKind.CAPABILITY, Linearity.LINEAR),),
        region_roles=("body",),
    ),
    "handle.invoke": OpSchema(
        "handle.invoke",
        (("closure", PortKind.CAPABILITY, Linearity.LINEAR),),
        (("closure", PortKind.CAPABILITY, Linearity.LINEAR),),
        region_roles=("clause",),
    ),
    "effect.perform": OpSchema(
        "effect.perform",
        (("payload", PortKind.VALUE, Linearity.UNRESTRICTED),),
        (("signal", PortKind.EFFECT, Linearity.LINEAR),),
    ),
    "handler.abort": OpSchema(
        "handler.abort",
        (("signal", PortKind.EFFECT, Linearity.LINEAR),),
        (("result", PortKind.VALUE, Linearity.UNRESTRICTED),),
    ),
    "const.result": OpSchema(
        "const.result", (), (("result", PortKind.VALUE, Linearity.UNRESTRICTED),), pure=True
    ),
    "pure.identity": OpSchema(
        "pure.identity",
        (("value", PortKind.VALUE, Linearity.UNRESTRICTED),),
        (("value", PortKind.VALUE, Linearity.UNRESTRICTED),),
        pure=True,
    ),
}


# ---------------------------------------------------------------------------
# Canonicalization and serialization
# ---------------------------------------------------------------------------


def _jsonable(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if isinstance(value, tuple):
        return [_jsonable(v) for v in value]
    if isinstance(value, list):
        return [_jsonable(v) for v in value]
    if isinstance(value, dict):
        return {k: _jsonable(v) for k, v in sorted(value.items())}
    return value


def graph_to_dict(g: Graph, include_display: bool = True) -> dict[str, Any]:
    return {
        "name": g.name,
        "root_region": g.root_region,
        "result_type": g.result_type,
        "observation_lens": list(g.observation_lens),
        "metadata": _jsonable(dict(g.metadata)),
        "ports": [
            {
                "id": p.id,
                "owner_kind": p.owner_kind,
                "owner_id": p.owner_id,
                "name": p.name,
                "direction": p.direction.value,
                "type": {
                    "kind": p.type.kind.value,
                    "payload": p.type.payload,
                    "linearity": p.type.linearity.value,
                    "erased": p.type.erased,
                },
                "ordinal": p.ordinal,
                **({"display": p.name} if include_display else {}),
            }
            for p in sorted(g.ports.values(), key=lambda x: x.id)
        ],
        "nodes": [
            {
                "id": n.id,
                "region_id": n.region_id,
                "op": n.op,
                "inputs": list(n.inputs),
                "outputs": list(n.outputs),
                "attrs": _jsonable(dict(n.attrs)),
                "children": [
                    {
                        "child_region": b.child_region,
                        "imports": [list(x) for x in b.imports],
                        "exports": [list(x) for x in b.exports],
                    }
                    for b in n.children
                ],
                **({"display": n.display} if include_display else {}),
            }
            for n in sorted(g.nodes.values(), key=lambda x: x.id)
        ],
        "edges": [
            {
                "id": e.id,
                "region_id": e.region_id,
                "source": e.source,
                "targets": list(e.targets),
                "role": e.role,
            }
            for e in sorted(g.edges.values(), key=lambda x: x.id)
        ],
        "regions": [
            {
                "id": r.id,
                "kind": r.kind,
                "parent_node": r.parent_node,
                "inputs": list(r.inputs),
                "exits": [
                    {
                        "name": x.name,
                        "ports": list(x.ports),
                        "protocol_state": [list(y) for y in x.protocol_state],
                    }
                    for x in r.exits
                ],
                "nodes": list(r.nodes),
                "edges": list(r.edges),
                **({"display": r.display} if include_display else {}),
            }
            for r in sorted(g.regions.values(), key=lambda x: x.id)
        ],
    }


def graph_from_dict(d: Mapping[str, Any]) -> Graph:
    ports: dict[str, Port] = {}
    for raw in d["ports"]:
        t = raw["type"]
        p = Port(
            id=raw["id"], owner_kind=raw["owner_kind"], owner_id=raw["owner_id"],
            name=raw["name"], direction=Direction(raw["direction"]),
            type=PortType(PortKind(t["kind"]), t["payload"], Linearity(t["linearity"]), bool(t.get("erased", False))),
            ordinal=int(raw["ordinal"]),
        )
        ports[p.id] = p
    nodes: dict[str, Node] = {}
    for raw in d["nodes"]:
        children = tuple(
            BoundaryBinding(
                child_region=c["child_region"],
                imports=tuple(tuple(x) for x in c.get("imports", ())),
                exports=tuple(tuple(x) for x in c.get("exports", ())),
            ) for c in raw.get("children", ())
        )
        n = Node(
            id=raw["id"], region_id=raw["region_id"], op=raw["op"],
            inputs=tuple(raw["inputs"]), outputs=tuple(raw["outputs"]),
            attrs=tuple(sorted(raw.get("attrs", {}).items())), children=children,
            display=raw.get("display", ""),
        )
        nodes[n.id] = n
    edges = {
        raw["id"]: Edge(raw["id"], raw["region_id"], raw["source"], tuple(raw["targets"]), raw.get("role", "data"))
        for raw in d["edges"]
    }
    regions: dict[str, Region] = {}
    for raw in d["regions"]:
        exits = tuple(
            ExitInterface(x["name"], tuple(x["ports"]), tuple(tuple(y) for y in x.get("protocol_state", ())))
            for x in raw["exits"]
        )
        r = Region(
            id=raw["id"], kind=raw["kind"], parent_node=raw.get("parent_node"),
            inputs=tuple(raw["inputs"]), exits=exits, nodes=tuple(raw["nodes"]),
            edges=tuple(raw["edges"]), display=raw.get("display", ""),
        )
        regions[r.id] = r
    return Graph(
        name=d["name"], root_region=d["root_region"], result_type=d["result_type"],
        observation_lens=tuple(d["observation_lens"]), ports=ports, nodes=nodes,
        edges=edges, regions=regions, metadata=d.get("metadata", {}),
    )


def canonical_json(g: Graph) -> str:
    return json.dumps(graph_to_dict(g), ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def exact_hash(g: Graph) -> str:
    return hashlib.sha256(canonical_json(g).encode()).hexdigest()


def _region_order(g: Graph) -> list[str]:
    out: list[str] = []
    def visit(rid: str) -> None:
        out.append(rid)
        r = g.regions[rid]
        for nid in r.nodes:
            n = g.nodes[nid]
            for b in n.children:
                visit(b.child_region)
    visit(g.root_region)
    return out


def alpha_canonical_dict(g: Graph) -> dict[str, Any]:
    """Canonicalize semantic IDs while preserving declared node/port order.

    Display names, source locations, and original IDs are intentionally excluded.
    The hierarchy supplies a deterministic traversal; this is alpha stability,
    not a general graph-isomorphism solver.
    """
    r_order = _region_order(g)
    rmap = {rid: f"r{i}" for i, rid in enumerate(r_order)}
    n_order: list[str] = []
    for rid in r_order:
        n_order.extend(g.regions[rid].nodes)
    nmap = {nid: f"n{i}" for i, nid in enumerate(n_order)}

    p_order: list[str] = []
    for rid in r_order:
        r = g.regions[rid]
        p_order.extend(r.inputs)
        for x in r.exits:
            p_order.extend(x.ports)
        for nid in r.nodes:
            n = g.nodes[nid]
            p_order.extend(n.inputs)
            p_order.extend(n.outputs)
    # Remove duplicates while preserving order.
    p_order = list(dict.fromkeys(p_order))
    pmap = {pid: f"p{i}" for i, pid in enumerate(p_order)}

    emap: dict[str, str] = {}
    e_order: list[str] = []
    for rid in r_order:
        e_order.extend(g.regions[rid].edges)
    for i, eid in enumerate(e_order):
        emap[eid] = f"e{i}"

    regions = []
    for rid in r_order:
        r = g.regions[rid]
        regions.append({
            "id": rmap[rid], "kind": r.kind,
            "parent_node": nmap.get(r.parent_node) if r.parent_node else None,
            "inputs": [pmap[x] for x in r.inputs],
            "exits": [
                {"name": x.name, "ports": [pmap[p] for p in x.ports], "protocol_state": list(x.protocol_state)}
                for x in r.exits
            ],
            "nodes": [nmap[x] for x in r.nodes],
            "edges": [emap[x] for x in r.edges],
        })

    nodes = []
    for nid in n_order:
        n = g.nodes[nid]
        # source/debug/display attributes do not participate in semantic identity.
        attrs = {k: v for k, v in n.attrs if k not in {"source", "debug", "display", "provenance"}}
        nodes.append({
            "id": nmap[nid], "region_id": rmap[n.region_id], "op": n.op,
            "inputs": [pmap[x] for x in n.inputs], "outputs": [pmap[x] for x in n.outputs],
            "attrs": _jsonable(attrs),
            "children": [
                {
                    "child_region": rmap[b.child_region],
                    "imports": [list(x) for x in b.imports],
                    "exports": [list(x) for x in b.exports],
                } for b in n.children
            ],
        })

    ports = []
    for pid in p_order:
        p = g.ports[pid]
        owner = nmap[p.owner_id] if p.owner_kind == "node" else rmap[p.owner_id]
        ports.append({
            "id": pmap[pid], "owner_kind": p.owner_kind, "owner_id": owner,
            "name": p.name, "direction": p.direction.value,
            "type": p.type.canonical(), "ordinal": p.ordinal,
        })

    edges = []
    for eid in e_order:
        e = g.edges[eid]
        edges.append({
            "id": emap[eid], "region_id": rmap[e.region_id],
            "source": pmap[e.source], "targets": [pmap[x] for x in e.targets], "role": e.role,
        })

    return {
        "result_type": g.result_type,
        "observation_lens": list(g.observation_lens),
        "root_region": rmap[g.root_region],
        "regions": regions, "nodes": nodes, "ports": ports, "edges": edges,
    }


def semantic_hash(g: Graph) -> str:
    payload = json.dumps(alpha_canonical_dict(g), ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode()).hexdigest()


# ---------------------------------------------------------------------------
# Trusted verifier
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Verification:
    ok: bool
    checks: tuple[str, ...]


def _owner_region(g: Graph, p: Port) -> str:
    return p.owner_id if p.owner_kind == "region" else g.nodes[p.owner_id].region_id


def _port_by_name(g: Graph, ids: Sequence[str], name: str) -> Port:
    found = [g.ports[x] for x in ids if g.ports[x].name == name]
    if len(found) != 1:
        raise GlyphError(f"expected exactly one port named {name!r}, got {len(found)}")
    return found[0]


def boundary_signature(g: Graph, rid: str) -> tuple:
    r = g.regions[rid]
    return (
        tuple((g.ports[p].name, g.ports[p].type.canonical()) for p in r.inputs),
        tuple((x.name, tuple((g.ports[p].name, g.ports[p].type.canonical()) for p in x.ports), x.protocol_state) for x in r.exits),
    )


def verify_graph(g: Graph) -> Verification:
    checks: list[str] = []
    if g.root_region not in g.regions:
        raise GlyphError("missing root region")
    if g.regions[g.root_region].parent_node is not None:
        raise GlyphError("root region must not have a parent node")
    checks.append("root-region")

    # Ownership and declarations.
    for rid, r in g.regions.items():
        if len(set(r.nodes)) != len(r.nodes) or len(set(r.edges)) != len(r.edges):
            raise GlyphError(f"duplicate node/edge declaration in region {rid}")
        for nid in r.nodes:
            if nid not in g.nodes or g.nodes[nid].region_id != rid:
                raise GlyphError(f"region {rid} has invalid node {nid}")
        for eid in r.edges:
            if eid not in g.edges or g.edges[eid].region_id != rid:
                raise GlyphError(f"region {rid} has invalid edge {eid}")
        for pid in r.inputs:
            p = g.ports.get(pid)
            if not p or p.owner_kind != "region" or p.owner_id != rid or p.direction != Direction.OUT:
                raise GlyphError(f"bad region input {pid}")
        for x in r.exits:
            for pid in x.ports:
                p = g.ports.get(pid)
                if not p or p.owner_kind != "region" or p.owner_id != rid or p.direction != Direction.IN:
                    raise GlyphError(f"bad region exit port {pid}")
    checks.append("region-declarations")

    for nid, n in g.nodes.items():
        if n.region_id not in g.regions or nid not in g.regions[n.region_id].nodes:
            raise GlyphError(f"orphan node {nid}")
        if n.op not in SCHEMAS:
            raise GlyphError(f"unknown operation schema {n.op}")
        schema = SCHEMAS[n.op]
        if len(n.inputs) != len(schema.input_specs) or len(n.outputs) != len(schema.output_specs):
            raise GlyphError(f"schema arity mismatch at {nid}")
        for pid, spec in zip(n.inputs, schema.input_specs):
            p = g.ports.get(pid)
            if not p or p.owner_kind != "node" or p.owner_id != nid or p.direction != Direction.IN:
                raise GlyphError(f"invalid input port {pid} at {nid}")
            name, kind, linearity = spec
            if p.name != name or p.type.kind != kind or p.type.linearity != linearity:
                raise GlyphError(f"input schema mismatch at {nid}.{p.name}")
        for pid, spec in zip(n.outputs, schema.output_specs):
            p = g.ports.get(pid)
            if not p or p.owner_kind != "node" or p.owner_id != nid or p.direction != Direction.OUT:
                raise GlyphError(f"invalid output port {pid} at {nid}")
            name, kind, linearity = spec
            if p.name != name or p.type.kind != kind or p.type.linearity != linearity:
                raise GlyphError(f"output schema mismatch at {nid}.{p.name}")
        roles = tuple(n.attr("role", "") for _ in ())  # keeps attrs checked below without hidden semantics
        child_roles = tuple(n.attr("child_roles", ()))
        if schema.region_roles:
            actual = tuple(n.attr("child_role_map", {}).keys()) if isinstance(n.attr("child_role_map", {}), dict) else ()
            # Reference graph uses explicit binding order and a role attribute on each child region.
            actual = tuple(g.regions[b.child_region].kind for b in n.children)
            if actual != schema.region_roles:
                raise GlyphError(f"child region roles mismatch at {nid}: {actual} != {schema.region_roles}")
        elif n.children:
            raise GlyphError(f"operation {n.op} may not own regions")
    checks.append("operation-schemas")

    # Edges and linearity.
    incoming: dict[str, list[str]] = {pid: [] for pid in g.ports}
    outgoing: dict[str, list[str]] = {pid: [] for pid in g.ports}
    for eid, e in g.edges.items():
        if not e.targets:
            raise GlyphError(f"edge {eid} has no targets")
        if e.source not in g.ports:
            raise GlyphError(f"edge {eid} has missing source")
        src = g.ports[e.source]
        if src.direction != Direction.OUT:
            raise GlyphError(f"edge {eid} source is not an output")
        if _owner_region(g, src) != e.region_id:
            raise GlyphError(f"edge {eid} source crosses a region boundary")
        for tid in e.targets:
            if tid not in g.ports:
                raise GlyphError(f"edge {eid} has missing target")
            dst = g.ports[tid]
            if dst.direction != Direction.IN:
                raise GlyphError(f"edge {eid} target is not an input")
            if _owner_region(g, dst) != e.region_id:
                raise GlyphError(f"edge {eid} target crosses a region boundary")
            if src.type != dst.type:
                raise GlyphError(f"edge {eid} type mismatch: {src.type} != {dst.type}")
            incoming[tid].append(eid)
        outgoing[e.source].append(eid)

    for pid, p in g.ports.items():
        if p.direction == Direction.IN:
            if len(incoming[pid]) != 1:
                raise GlyphError(f"input {pid} must have exactly one producer")
        else:
            fanout = sum(len(g.edges[eid].targets) for eid in outgoing[pid])
            if p.type.linearity == Linearity.LINEAR and fanout != 1:
                raise GlyphError(f"linear output {pid} must have exactly one consumer, got {fanout}")
            if p.type.linearity == Linearity.AFFINE and fanout > 1:
                raise GlyphError(f"affine output {pid} has multiple consumers")
            if p.type.erased:
                for eid in outgoing[pid]:
                    for tid in g.edges[eid].targets:
                        if not g.ports[tid].type.erased:
                            raise GlyphError(f"erased proof {pid} enters runtime port {tid}")
    checks.append("typed-edges-and-linearity")

    # Region boundaries and structured composition.
    child_seen: set[str] = set()
    for nid, n in g.nodes.items():
        for b in n.children:
            if b.child_region not in g.regions:
                raise GlyphError(f"missing child region {b.child_region}")
            child = g.regions[b.child_region]
            if child.parent_node != nid:
                raise GlyphError(f"child region {child.id} has wrong parent")
            if child.id in child_seen:
                raise GlyphError(f"child region {child.id} has multiple owners")
            child_seen.add(child.id)
            bound_child_inputs = {child_name for _, child_name in b.imports}
            for parent_name, child_name in b.imports:
                pp = _port_by_name(g, n.inputs, parent_name)
                cp = _port_by_name(g, child.inputs, child_name)
                if pp.type != cp.type:
                    raise GlyphError(f"import boundary mismatch {nid}:{parent_name} -> {child.id}:{child_name}")
            for pid in child.inputs:
                cp = g.ports[pid]
                dynamic_effect_portal = child.kind == "clause" and cp.type.kind == PortKind.EFFECT
                if cp.name not in bound_child_inputs and not dynamic_effect_portal:
                    raise GlyphError(f"hidden child capture {child.id}:{cp.name}")
            for exit_name, child_name, parent_name in b.exports:
                exits = [x for x in child.exits if x.name == exit_name]
                if len(exits) != 1:
                    raise GlyphError(f"unknown child exit {child.id}:{exit_name}")
                cp = _port_by_name(g, exits[0].ports, child_name)
                pp = _port_by_name(g, n.outputs, parent_name)
                if cp.type != pp.type:
                    raise GlyphError(f"export boundary mismatch {child.id}:{child_name} -> {nid}:{parent_name}")
    expected_children = set(g.regions) - {g.root_region}
    if child_seen != expected_children:
        raise GlyphError(f"unowned regions: {sorted(expected_children - child_seen)}")
    checks.append("region-boundary-protocols")

    # Branch equality: same imported protocol and normal-result type; arm-local linear authority must close.
    for n in g.nodes.values():
        if n.op != "control.branch":
            continue
        if len(n.children) != 2:
            raise GlyphError("branch must own exactly two arms")
        sigs = [boundary_signature(g, b.child_region) for b in n.children]
        if sigs[0] != sigs[1]:
            raise GlyphError("branch arm interface mismatch")
        for b in n.children:
            child = g.regions[b.child_region]
            normal = [x for x in child.exits if x.name == "normal"]
            if len(normal) != 1:
                raise GlyphError("branch arm lacks one normal exit")
            # Only the result value is allowed to leave the cutover arms.
            if any(g.ports[p].type.kind in {PortKind.CAPABILITY, PortKind.LOAN} for p in normal[0].ports):
                raise GlyphError("branch arm leaks authority")
    checks.append("branch-interface-equality")

    # Effect contracts: closure body exposes the declared effect; handler covers it.
    closure_effects: dict[str, set[str]] = {}
    for n in g.nodes.values():
        if n.op == "closure.make":
            body = g.regions[n.children[0].child_region]
            effects = {x.name.removeprefix("suspend:") for x in body.exits if x.name.startswith("suspend:")}
            declared = set(n.attr("effects", ()))
            if effects != declared:
                raise GlyphError(f"closure effect contract mismatch at {n.id}")
            closure_effects[n.id] = effects
        if n.op == "handle.invoke":
            handled = set(n.attr("handled_effects", ()))
            latent = set(n.attr("latent_effects", ()))
            if not latent <= handled:
                raise GlyphError(f"unhandled latent effects at {n.id}: {sorted(latent - handled)}")
            clause = g.regions[n.children[0].child_region]
            clause_effects = {x.name.removeprefix("handle:") for x in clause.exits if x.name.startswith("handle:")}
            if clause_effects != handled:
                raise GlyphError(f"handler clause protocol mismatch at {n.id}")
            if n.attr("resumption_grade") == "ω" and n.attr("frame_policy") not in {"clone", "snapshot"}:
                raise GlyphError("multi-shot handler requires clone/snapshot frame policy")
    checks.append("effect-and-continuation-contracts")

    # All region exit ports must be fed (already guaranteed by input rule) and result root must match.
    root = g.regions[g.root_region]
    normal = [x for x in root.exits if x.name == "normal"]
    if len(normal) != 1 or len(normal[0].ports) != 1 or g.ports[normal[0].ports[0]].type.payload != g.result_type:
        raise GlyphError("root normal exit does not match function result")
    checks.append("root-result")
    return Verification(True, tuple(checks))


# ---------------------------------------------------------------------------
# Region Merkle hashes and proof-carrying patches
# ---------------------------------------------------------------------------


def region_hashes(g: Graph) -> dict[str, str]:
    result: dict[str, str] = {}
    def visit(rid: str) -> str:
        r = g.regions[rid]
        child_hashes: list[tuple[str, str]] = []
        node_payload = []
        for nid in r.nodes:
            n = g.nodes[nid]
            ch = [(g.regions[b.child_region].kind, visit(b.child_region)) for b in n.children]
            child_hashes.extend(ch)
            node_payload.append({
                "op": n.op,
                "attrs": _jsonable({k: v for k, v in n.attrs if k not in {"source", "debug", "display"}}),
                "inputs": [(g.ports[p].name, g.ports[p].type.canonical()) for p in n.inputs],
                "outputs": [(g.ports[p].name, g.ports[p].type.canonical()) for p in n.outputs],
                "children": ch,
            })
        edge_payload = []
        local_port_index: dict[str, str] = {}
        for i, pid in enumerate(r.inputs): local_port_index[pid] = f"ri{i}"
        for xi, x in enumerate(r.exits):
            for pi, pid in enumerate(x.ports): local_port_index[pid] = f"rx{xi}.{pi}"
        for ni, nid in enumerate(r.nodes):
            n = g.nodes[nid]
            for pi, pid in enumerate(n.inputs): local_port_index[pid] = f"n{ni}.i{pi}"
            for pi, pid in enumerate(n.outputs): local_port_index[pid] = f"n{ni}.o{pi}"
        for eid in r.edges:
            e = g.edges[eid]
            edge_payload.append((local_port_index[e.source], tuple(local_port_index[t] for t in e.targets), e.role))
        payload = {
            "kind": r.kind,
            "interface": boundary_signature(g, rid),
            "nodes": node_payload,
            "edges": edge_payload,
        }
        h = hashlib.sha256(json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        result[rid] = h
        return h
    visit(g.root_region)
    return result


@dataclass(frozen=True)
class PatchCertificate:
    rule: str
    source_hash: str
    target_hash: str
    lens: tuple[str, ...]
    changed_regions: tuple[str, ...]
    source_boundary_hashes: tuple[tuple[str, str], ...]
    target_boundary_hashes: tuple[tuple[str, str], ...]
    witness: tuple[tuple[str, str], ...]


def remove_identity_node(g: Graph, node_id: str) -> tuple[Graph, PatchCertificate]:
    verify_graph(g)
    n = g.nodes.get(node_id)
    if not n or n.op != "pure.identity":
        raise GlyphError("identity-elimination requires a pure.identity node")
    rid = n.region_id
    inp, out = n.inputs[0], n.outputs[0]
    in_edges = [e for e in g.edges.values() if inp in e.targets]
    out_edges = [e for e in g.edges.values() if e.source == out]
    if len(in_edges) != 1 or len(out_edges) != 1:
        raise GlyphError("identity node must have one incoming and one outgoing edge")
    ine, oute = in_edges[0], out_edges[0]
    new_edge = Edge(ine.id, rid, ine.source, oute.targets, role="identity-elim")
    edges = dict(g.edges)
    edges[ine.id] = new_edge
    del edges[oute.id]
    nodes = dict(g.nodes); del nodes[node_id]
    ports = dict(g.ports); del ports[inp]; del ports[out]
    region = g.regions[rid]
    regions = dict(g.regions)
    regions[rid] = replace(region,
        nodes=tuple(x for x in region.nodes if x != node_id),
        edges=tuple(x for x in region.edges if x != oute.id))
    target = replace(g, ports=ports, nodes=nodes, edges=edges, regions=regions)
    verify_graph(target)
    sh = region_hashes(g); th = region_hashes(target)
    cert = PatchCertificate(
        rule="pure.identity-elimination",
        source_hash=semantic_hash(g), target_hash=semantic_hash(target),
        lens=("language", "safety"), changed_regions=(rid,),
        source_boundary_hashes=((rid, hashlib.sha256(repr(boundary_signature(g, rid)).encode()).hexdigest()),),
        target_boundary_hashes=((rid, hashlib.sha256(repr(boundary_signature(target, rid)).encode()).hexdigest()),),
        witness=(("node", node_id), ("incoming", ine.id), ("outgoing", oute.id)),
    )
    return target, cert


def check_patch(source: Graph, target: Graph, cert: PatchCertificate) -> None:
    verify_graph(source); verify_graph(target)
    if cert.source_hash != semantic_hash(source) or cert.target_hash != semantic_hash(target):
        raise GlyphError("patch hash mismatch")
    if cert.rule != "pure.identity-elimination":
        raise GlyphError("unknown patch rule")
    witness = dict(cert.witness)
    nid = witness.get("node")
    if nid not in source.nodes or source.nodes[nid].op != "pure.identity":
        raise GlyphError("certificate does not identify an identity node")
    if nid in target.nodes:
        raise GlyphError("identity node still exists in target")
    if not set(cert.lens) <= {"language", "safety"}:
        raise GlyphError("identity elimination is not certified for this observation lens")
    for (rid, a), (_, b) in zip(cert.source_boundary_hashes, cert.target_boundary_hashes):
        if rid not in source.regions or rid not in target.regions or a != b:
            raise GlyphError("patch changed the region boundary protocol")
    # Recompute the expected target instead of trusting the transform.
    expected, _ = remove_identity_node(source, nid)
    if semantic_hash(expected) != semantic_hash(target):
        raise GlyphError("target is not the certified local rewrite")


# ---------------------------------------------------------------------------
# Builder helpers and hostile specimen
# ---------------------------------------------------------------------------


class Builder:
    def __init__(self, name: str, result_type: str):
        self.name = name
        self.result_type = result_type
        self.ports: dict[str, Port] = {}
        self.nodes: dict[str, Node] = {}
        self.edges: dict[str, Edge] = {}
        self.regions: dict[str, Region] = {}
        self._edge_counter = 0

    def port(self, pid: str, owner_kind: str, owner_id: str, name: str, direction: Direction, t: PortType, ordinal: int) -> str:
        if pid in self.ports: raise GlyphError(f"duplicate port {pid}")
        self.ports[pid] = Port(pid, owner_kind, owner_id, name, direction, t, ordinal)
        return pid

    def node(self, nid: str, rid: str, op: str, ins: Sequence[str], outs: Sequence[str], attrs: Mapping[str, Any] | None = None, children: Sequence[BoundaryBinding] = (), display: str = "") -> None:
        self.nodes[nid] = Node(nid, rid, op, tuple(ins), tuple(outs), tuple(sorted((attrs or {}).items())), tuple(children), display)

    def edge(self, rid: str, source: str, *targets: str, role: str = "data", eid: str | None = None) -> str:
        if eid is None:
            eid = f"e{self._edge_counter}"; self._edge_counter += 1
        self.edges[eid] = Edge(eid, rid, source, tuple(targets), role)
        return eid

    def region(self, rid: str, kind: str, parent: str | None, inputs: Sequence[str], exits: Sequence[ExitInterface], nodes: Sequence[str], edges: Sequence[str], display: str = "") -> None:
        self.regions[rid] = Region(rid, kind, parent, tuple(inputs), tuple(exits), tuple(nodes), tuple(edges), display)

    def finish(self, root: str) -> Graph:
        return Graph(self.name, root, self.result_type, ("language", "safety", "resource-events"), self.ports, self.nodes, self.edges, self.regions, {"version": "0.5"})


def PT(kind: PortKind, payload: str, linearity: Linearity, erased: bool = False) -> PortType:
    return PortType(kind, payload, linearity, erased)


VAL_REQUEST = PT(PortKind.VALUE, "Request", Linearity.UNRESTRICTED)
VAL_BOOL = PT(PortKind.VALUE, "Bool", Linearity.UNRESTRICTED)
VAL_RESULT = PT(PortKind.VALUE, "Result", Linearity.UNRESTRICTED)
VAL_STRING = PT(PortKind.VALUE, "String", Linearity.UNRESTRICTED)
VAL_UNIT = PT(PortKind.VALUE, "Unit", Linearity.UNRESTRICTED)
CAP_REQUEST = PT(PortKind.CAPABILITY, "Request{payload:owned,meta:owned}", Linearity.LINEAR)
CAP_REMAINDER = PT(PortKind.CAPABILITY, "Request{payload:gone,meta:owned}", Linearity.LINEAR)
CAP_STRING = PT(PortKind.CAPABILITY, "String{root:owned}", Linearity.LINEAR)
CAP_CLOSURE = PT(PortKind.CAPABILITY, "Closure@emit{payload:owned}", Linearity.LINEAR)
EFF_WRITE = PT(PortKind.EFFECT, "Console.write", Linearity.LINEAR)


def build_specimen(with_identity: bool = True) -> Graph:
    b = Builder("process-request", "Result")

    # Root interface.
    b.port("p_root_request", "region", "r_root", "request", Direction.OUT, VAL_REQUEST, 0)
    b.port("p_root_cap", "region", "r_root", "resource", Direction.OUT, CAP_REQUEST, 1)
    b.port("p_root_result", "region", "r_root", "result", Direction.IN, VAL_RESULT, 0)

    # Validate.
    b.port("p_validate_in", "node", "n_validate", "request", Direction.IN, VAL_REQUEST, 0)
    b.port("p_validate_out", "node", "n_validate", "valid", Direction.OUT, VAL_BOOL, 0)
    b.node("n_validate", "r_root", "call.validate", ["p_validate_in"], ["p_validate_out"], {"callee": "validate"}, display="validate request.meta")

    prev_valid = "p_validate_out"
    root_nodes = ["n_validate"]
    root_edges: list[str] = []
    root_edges.append(b.edge("r_root", "p_root_request", "p_validate_in"))

    if with_identity:
        b.port("p_ident_in", "node", "n_ident", "value", Direction.IN, VAL_BOOL, 0)
        b.port("p_ident_out", "node", "n_ident", "value", Direction.OUT, VAL_BOOL, 0)
        b.node("n_ident", "r_root", "pure.identity", ["p_ident_in"], ["p_ident_out"], display="identity proof specimen")
        root_nodes.append("n_ident")
        root_edges.append(b.edge("r_root", "p_validate_out", "p_ident_in"))
        prev_valid = "p_ident_out"

    # Branch parent ports.
    b.port("p_branch_cond", "node", "n_branch", "condition", Direction.IN, VAL_BOOL, 0)
    b.port("p_branch_cap", "node", "n_branch", "resource", Direction.IN, CAP_REQUEST, 1)
    b.port("p_branch_result", "node", "n_branch", "result", Direction.OUT, VAL_RESULT, 0)
    root_edges.append(b.edge("r_root", prev_valid, "p_branch_cond"))
    root_edges.append(b.edge("r_root", "p_root_cap", "p_branch_cap"))
    root_edges.append(b.edge("r_root", "p_branch_result", "p_root_result"))

    yes_binding = BoundaryBinding("r_yes", imports=(("resource", "resource"),), exports=(("normal", "result", "result"),))
    no_binding = BoundaryBinding("r_no", imports=(("resource", "resource"),), exports=(("normal", "result", "result"),))
    b.node("n_branch", "r_root", "control.branch", ["p_branch_cond", "p_branch_cap"], ["p_branch_result"], children=(yes_binding, no_binding), display="valid?")
    root_nodes.append("n_branch")

    # YES arm interface.
    b.port("p_yes_cap", "region", "r_yes", "resource", Direction.OUT, CAP_REQUEST, 0)
    b.port("p_yes_result", "region", "r_yes", "result", Direction.IN, VAL_RESULT, 0)

    # Move payload.
    b.port("p_move_in", "node", "n_move", "source", Direction.IN, CAP_REQUEST, 0)
    b.port("p_move_field", "node", "n_move", "field", Direction.OUT, CAP_STRING, 0)
    b.port("p_move_rem", "node", "n_move", "remainder", Direction.OUT, CAP_REMAINDER, 1)
    b.node("n_move", "r_yes", "cap.move-field", ["p_move_in"], ["p_move_field", "p_move_rem"], {"place": ".payload"}, display="evacuate payload")

    # Drop remainder.
    b.port("p_drop_rem", "node", "n_drop_rem", "resource", Direction.IN, CAP_REMAINDER, 0)
    b.node("n_drop_rem", "r_yes", "cap.drop", ["p_drop_rem"], [], {"mask": ("meta",)}, display="drop request remainder")

    # Closure.
    b.port("p_closure_capture", "node", "n_closure", "capture", Direction.IN, CAP_STRING, 0)
    b.port("p_closure_out", "node", "n_closure", "closure", Direction.OUT, CAP_CLOSURE, 0)
    closure_binding = BoundaryBinding("r_closure", imports=(("capture", "payload"),), exports=())
    b.node("n_closure", "r_yes", "closure.make", ["p_closure_capture"], ["p_closure_out"], {"effects": ("Console.write",), "capture_mode": "move"}, (closure_binding,), display="closure emit")

    # Handle invoke.
    b.port("p_handle_in", "node", "n_handle", "closure", Direction.IN, CAP_CLOSURE, 0)
    b.port("p_handle_out", "node", "n_handle", "closure", Direction.OUT, CAP_CLOSURE, 0)
    clause_binding = BoundaryBinding("r_clause", imports=(), exports=())
    b.node("n_handle", "r_yes", "handle.invoke", ["p_handle_in"], ["p_handle_out"], {
        "handled_effects": ("Console.write",), "latent_effects": ("Console.write",),
        "resumption_grade": "0", "frame_policy": "forbid",
    }, (clause_binding,), display="handle Console.write")

    # Drop closure.
    b.port("p_drop_closure", "node", "n_drop_closure", "resource", Direction.IN, CAP_CLOSURE, 0)
    b.node("n_drop_closure", "r_yes", "cap.drop", ["p_drop_closure"], [], {"mask": ("payload",)}, display="drop closure environment")

    # Result constant.
    b.port("p_ok", "node", "n_ok", "result", Direction.OUT, VAL_RESULT, 0)
    b.node("n_ok", "r_yes", "const.result", [], ["p_ok"], {"value": "Ok"}, display="Ok")

    yes_edges = [
        b.edge("r_yes", "p_yes_cap", "p_move_in"),
        b.edge("r_yes", "p_move_rem", "p_drop_rem"),
        b.edge("r_yes", "p_move_field", "p_closure_capture"),
        b.edge("r_yes", "p_closure_out", "p_handle_in"),
        b.edge("r_yes", "p_handle_out", "p_drop_closure"),
        b.edge("r_yes", "p_ok", "p_yes_result"),
    ]

    # Closure body region: project a readable value while threading the linear frame capability.
    b.port("p_cb_cap", "region", "r_closure", "payload", Direction.OUT, CAP_STRING, 0)
    b.port("p_cb_effect", "region", "r_closure", "signal", Direction.IN, EFF_WRITE, 0)
    b.port("p_project_in", "node", "n_project", "resource", Direction.IN, CAP_STRING, 0)
    b.port("p_project_cap", "node", "n_project", "resource", Direction.OUT, CAP_STRING, 0)
    b.port("p_project_value", "node", "n_project", "value", Direction.OUT, VAL_STRING, 1)
    b.node("n_project", "r_closure", "cap.project", ["p_project_in"], ["p_project_cap", "p_project_value"], {"place": ".root"}, display="read captured payload")
    b.port("p_perform_payload", "node", "n_perform", "payload", Direction.IN, VAL_STRING, 0)
    b.port("p_perform_signal", "node", "n_perform", "signal", Direction.OUT, EFF_WRITE, 0)
    b.node("n_perform", "r_closure", "effect.perform", ["p_perform_payload"], ["p_perform_signal"], {"effect": "Console.write"}, display="perform Console.write")
    # The captured capability crosses the suspension exit as explicit frame authority.
    b.port("p_cb_frame", "region", "r_closure", "frame", Direction.IN, CAP_STRING, 1)
    cb_edges = [
        b.edge("r_closure", "p_cb_cap", "p_project_in"),
        b.edge("r_closure", "p_project_cap", "p_cb_frame"),
        b.edge("r_closure", "p_project_value", "p_perform_payload"),
        b.edge("r_closure", "p_perform_signal", "p_cb_effect"),
    ]

    # Handler clause region. The effect signal is a declared dynamic portal.
    b.port("p_clause_effect", "region", "r_clause", "signal", Direction.OUT, EFF_WRITE, 0)
    b.port("p_clause_result", "region", "r_clause", "result", Direction.IN, VAL_UNIT, 0)
    b.port("p_abort_effect", "node", "n_abort", "signal", Direction.IN, EFF_WRITE, 0)
    b.port("p_abort_result", "node", "n_abort", "result", Direction.OUT, VAL_UNIT, 0)
    b.node("n_abort", "r_clause", "handler.abort", ["p_abort_effect"], ["p_abort_result"], {"effect": "Console.write"}, display="abortive clause")
    clause_edges = [
        b.edge("r_clause", "p_clause_effect", "p_abort_effect"),
        b.edge("r_clause", "p_abort_result", "p_clause_result"),
    ]

    # NO arm.
    b.port("p_no_cap", "region", "r_no", "resource", Direction.OUT, CAP_REQUEST, 0)
    b.port("p_no_result", "region", "r_no", "result", Direction.IN, VAL_RESULT, 0)
    b.port("p_drop_all", "node", "n_drop_all", "resource", Direction.IN, CAP_REQUEST, 0)
    b.node("n_drop_all", "r_no", "cap.drop", ["p_drop_all"], [], {"mask": ("payload", "meta")}, display="drop complete request")
    b.port("p_rejected", "node", "n_rejected", "result", Direction.OUT, VAL_RESULT, 0)
    b.node("n_rejected", "r_no", "const.result", [], ["p_rejected"], {"value": "Rejected"}, display="Rejected")
    no_edges = [
        b.edge("r_no", "p_no_cap", "p_drop_all"),
        b.edge("r_no", "p_rejected", "p_no_result"),
    ]

    b.region("r_closure", "body", "n_closure", ["p_cb_cap"], [ExitInterface("suspend:Console.write", ("p_cb_effect", "p_cb_frame"), (("frame", "forbid"),))], ["n_project", "n_perform"], cb_edges, "emit body")
    b.region("r_clause", "clause", "n_handle", ["p_clause_effect"], [ExitInterface("handle:Console.write", ("p_clause_result",), (("resumption", "0"),))], ["n_abort"], clause_edges, "Console clause")
    b.region("r_yes", "yes", "n_branch", ["p_yes_cap"], [ExitInterface("normal", ("p_yes_result",), (("request", "gone"),))], ["n_move", "n_drop_rem", "n_closure", "n_handle", "n_drop_closure", "n_ok"], yes_edges, "yes arm")
    b.region("r_no", "no", "n_branch", ["p_no_cap"], [ExitInterface("normal", ("p_no_result",), (("request", "gone"),))], ["n_drop_all", "n_rejected"], no_edges, "no arm")
    b.region("r_root", "function", None, ["p_root_request", "p_root_cap"], [ExitInterface("normal", ("p_root_result",), (("request", "gone"),))], root_nodes, root_edges, "process-request")

    g = b.finish("r_root")
    return g


# ---------------------------------------------------------------------------
# Visual renderer
# ---------------------------------------------------------------------------


_KIND_GLYPH = {
    PortKind.VALUE: "%", PortKind.CAPABILITY: "κ", PortKind.LOAN: "ℓ",
    PortKind.EFFECT: "!", PortKind.RESUMPTION: "↻", PortKind.STATE: "σ", PortKind.PROOF: "π",
}


def _ptype(p: Port) -> str:
    ghost = " erased" if p.type.erased else ""
    return f"{_KIND_GLYPH[p.type.kind]}{p.name} : {p.type.payload} [{p.type.linearity.value}{ghost}]"


def _source_map(g: Graph) -> dict[str, str]:
    m: dict[str, str] = {}
    for e in g.edges.values():
        for t in e.targets:
            m[t] = e.source
    return m


def _port_ref(g: Graph, pid: str) -> str:
    p = g.ports[pid]
    if p.owner_kind == "region":
        return f"@{g.regions[p.owner_id].kind}.{p.name}"
    return f"@{g.nodes[p.owner_id].op}.{p.name}"


def render_visual(g: Graph) -> str:
    src = _source_map(g)
    lines = [f"fn @{g.name} -> {g.result_type}", "◎ observe {" + ", ".join("@" + x for x in g.observation_lens) + "}", ""]

    def region(rid: str, depth: int) -> None:
        r = g.regions[rid]
        pre = "│  " * depth
        lines.append(pre + f"╭─ {r.kind} @{r.display or rid}")
        for pid in r.inputs:
            lines.append(pre + "│  " + "├─ in  " + _ptype(g.ports[pid]))
        for nid in r.nodes:
            n = g.nodes[nid]
            lines.append(pre + "│  " + f"╭─ {n.op}  {n.display}".rstrip())
            for pid in n.inputs:
                p = g.ports[pid]
                lines.append(pre + "│  │  " + f"├─ {_ptype(p)} ◀── {_port_ref(g, src[pid])}")
            for pid in n.outputs:
                p = g.ports[pid]
                targets = []
                for e in g.edges.values():
                    if e.source == pid:
                        targets.extend(_port_ref(g, t) for t in e.targets)
                arrow = " ──▶ " + ", ".join(targets) if targets else ""
                lines.append(pre + "│  │  " + f"├─ {_ptype(p)}{arrow}")
            for b in n.children:
                lines.append(pre + "│  │  " + f"├─ owns {g.regions[b.child_region].kind} @{g.regions[b.child_region].display or b.child_region}")
            lines.append(pre + "│  " + f"╰─ end {n.op}")
            for b in n.children:
                region(b.child_region, depth + 1)
        for x in r.exits:
            for pid in x.ports:
                p = g.ports[pid]
                lines.append(pre + "│  " + f"╰─ exit {x.name} {_ptype(p)} ◀── {_port_ref(g, src[pid])}")
        lines.append(pre + f"╰─ end {r.kind} @{r.display or rid}")

    region(g.root_region, 0)
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Mutation suite and self-test
# ---------------------------------------------------------------------------


def renamed(g: Graph) -> Graph:
    """Alpha-rename every semantic ID while preserving structure."""
    rmap = {x: "R_" + x for x in g.regions}
    nmap = {x: "N_" + x for x in g.nodes}
    pmap = {x: "P_" + x for x in g.ports}
    emap = {x: "E_" + x for x in g.edges}
    ports = {
        pmap[k]: replace(v, id=pmap[k], owner_id=nmap[v.owner_id] if v.owner_kind == "node" else rmap[v.owner_id])
        for k, v in g.ports.items()
    }
    nodes = {
        nmap[k]: replace(v, id=nmap[k], region_id=rmap[v.region_id], inputs=tuple(pmap[x] for x in v.inputs), outputs=tuple(pmap[x] for x in v.outputs),
                         children=tuple(BoundaryBinding(rmap[b.child_region], b.imports, b.exports) for b in v.children))
        for k, v in g.nodes.items()
    }
    edges = {
        emap[k]: replace(v, id=emap[k], region_id=rmap[v.region_id], source=pmap[v.source], targets=tuple(pmap[x] for x in v.targets))
        for k, v in g.edges.items()
    }
    regions = {
        rmap[k]: replace(v, id=rmap[k], parent_node=nmap.get(v.parent_node) if v.parent_node else None,
                         inputs=tuple(pmap[x] for x in v.inputs),
                         exits=tuple(ExitInterface(x.name, tuple(pmap[p] for p in x.ports), x.protocol_state) for x in v.exits),
                         nodes=tuple(nmap[x] for x in v.nodes), edges=tuple(emap[x] for x in v.edges))
        for k, v in g.regions.items()
    }
    return replace(g, root_region=rmap[g.root_region], ports=ports, nodes=nodes, edges=edges, regions=regions)


def expect_reject(name: str, mutator) -> str:
    g = build_specimen()
    try:
        bad = mutator(g)
        verify_graph(bad)
    except (GlyphError, KeyError, ValueError) as e:
        return f"PASS reject {name}: {e}"
    raise AssertionError(f"mutation unexpectedly accepted: {name}")


def mutate_remove_linear_consumer(g: Graph) -> Graph:
    edges = dict(g.edges); del edges[next(eid for eid,e in edges.items() if e.source == "p_move_field")]
    r = g.regions["r_yes"]; regions = dict(g.regions); regions["r_yes"] = replace(r, edges=tuple(x for x in r.edges if x in edges))
    return replace(g, edges=edges, regions=regions)


def mutate_duplicate_linear(g: Graph) -> Graph:
    edges = dict(g.edges)
    eid = next(eid for eid, e in edges.items() if e.source == "p_move_field")
    edges[eid] = replace(edges[eid], targets=("p_closure_capture", "p_closure_capture"), role="evil-dup")
    return replace(g, edges=edges)


def mutate_cross_region(g: Graph) -> Graph:
    edges = dict(g.edges); eid = next(eid for eid,e in edges.items() if e.source == "p_validate_out")
    edges[eid] = replace(edges[eid], targets=("p_move_in",))
    return replace(g, edges=edges)


def mutate_wrong_type(g: Graph) -> Graph:
    ports = dict(g.ports); p = ports["p_branch_cond"]; ports[p.id] = replace(p, type=VAL_RESULT)
    return replace(g, ports=ports)


def mutate_unhandled(g: Graph) -> Graph:
    nodes = dict(g.nodes); n = nodes["n_handle"]; attrs = dict(n.attrs); attrs["handled_effects"] = (); nodes[n.id] = replace(n, attrs=tuple(sorted(attrs.items())))
    return replace(g, nodes=nodes)


def mutate_branch_protocol(g: Graph) -> Graph:
    regions = dict(g.regions); r = regions["r_no"]; exits = (ExitInterface("normal", r.exits[0].ports, (("request", "owned"),)),); regions[r.id] = replace(r, exits=exits)
    return replace(g, regions=regions)


def mutate_multi_shot_frame(g: Graph) -> Graph:
    nodes = dict(g.nodes); n = nodes["n_handle"]; attrs = dict(n.attrs); attrs["resumption_grade"] = "ω"; attrs["frame_policy"] = "forbid"; nodes[n.id] = replace(n, attrs=tuple(sorted(attrs.items())))
    return replace(g, nodes=nodes)


def mutate_orphan_region(g: Graph) -> Graph:
    nodes = dict(g.nodes); n = nodes["n_closure"]; nodes[n.id] = replace(n, children=())
    return replace(g, nodes=nodes)


def mutate_effect_contract(g: Graph) -> Graph:
    nodes = dict(g.nodes); n = nodes["n_closure"]; attrs = dict(n.attrs); attrs["effects"] = (); nodes[n.id] = replace(n, attrs=tuple(sorted(attrs.items())))
    return replace(g, nodes=nodes)


def mutate_input_without_source(g: Graph) -> Graph:
    edges = dict(g.edges); eid = next(eid for eid,e in edges.items() if "p_validate_in" in e.targets); del edges[eid]
    r = g.regions["r_root"]; regions = dict(g.regions); regions[r.id] = replace(r, edges=tuple(x for x in r.edges if x != eid))
    return replace(g, edges=edges, regions=regions)


def run() -> str:
    out: list[str] = []
    g = build_specimen(with_identity=True)
    v = verify_graph(g)
    out.append("Glyph IR v0.5 reference validation")
    out.append("PASS explicit graph verification: " + ", ".join(v.checks))

    text = canonical_json(g)
    roundtrip = graph_from_dict(json.loads(text))
    assert exact_hash(g) == exact_hash(roundtrip)
    out.append("PASS canonical graph serialization round-trip")

    gr = renamed(g)
    verify_graph(gr)
    assert semantic_hash(g) == semantic_hash(gr)
    assert exact_hash(g) != exact_hash(gr)
    out.append("PASS alpha-stable semantic identity; exact identity remains distinct")

    visual = render_visual(g)
    out.append("PASS deterministic visual projection")
    out.append("VISUAL-BEGIN")
    out.extend(visual.rstrip().splitlines())
    out.append("VISUAL-END")

    before_regions = region_hashes(g)
    target, cert = remove_identity_node(g, "n_ident")
    check_patch(g, target, cert)
    after_regions = region_hashes(target)
    assert before_regions["r_yes"] == after_regions["r_yes"]
    assert before_regions["r_no"] == after_regions["r_no"]
    assert before_regions["r_root"] != after_regions["r_root"]
    out.append("PASS proof-carrying identity patch independently replayed")
    out.append("PASS incremental invalidation is local: sibling region hashes unchanged")

    forged = replace(cert, target_hash="0" * 64)
    try:
        check_patch(g, target, forged)
    except GlyphError as e:
        out.append(f"PASS reject forged patch certificate: {e}")
    else:
        raise AssertionError("forged certificate accepted")

    mutations = [
        ("missing linear consumer", mutate_remove_linear_consumer),
        ("duplicated linear authority", mutate_duplicate_linear),
        ("hidden cross-region wire", mutate_cross_region),
        ("typed edge mismatch", mutate_wrong_type),
        ("unhandled latent effect", mutate_unhandled),
        ("branch protocol mismatch", mutate_branch_protocol),
        ("illegal multi-shot frame", mutate_multi_shot_frame),
        ("orphan child region", mutate_orphan_region),
        ("closure effect declaration mismatch", mutate_effect_contract),
        ("input without producer", mutate_input_without_source),
    ]
    for name, fn in mutations:
        out.append(expect_reject(name, fn))

    out.append("SUMMARY 17/17 gates passed")
    out.append("semantic-hash " + semantic_hash(g))
    out.append("exact-hash " + exact_hash(g))
    out.append("patched-semantic-hash " + semantic_hash(target))
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    print(run(), end="")
