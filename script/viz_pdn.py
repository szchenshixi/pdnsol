#!/usr/bin/env python3
import argparse
import json
import math
import re
from collections import defaultdict

import plotly.graph_objects as go


class DSU:
    def __init__(self, items):
        self.parent = {x: x for x in items}
        self.rank = {x: 0 for x in items}

    def find(self, x):
        # Path compression
        p = self.parent.get(x, x)
        if p != x:
            self.parent[x] = self.find(p)
        return self.parent.get(x, x)

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return
        if self.rank[ra] < self.rank[rb]:
            ra, rb = rb, ra
        self.parent[rb] = ra
        if self.rank[ra] == self.rank[rb]:
            self.rank[ra] += 1


def layer_rank(name: str) -> int:
    """
    Best-effort numeric rank from layer names like M1/M2/met3/M10.
    If not parseable, returns 0.
    """
    if not name:
        return 0
    m = re.search(r"(\d+)", name)
    return int(m.group(1)) if m else 0


def fmt_eng(value, unit: str = "", sig: int = 4) -> str:
    """
    Engineering-format a number with SI prefixes.

    Examples:
      0.00012 Ω -> 120 µΩ
      12.3 Ω    -> 12.3 Ω
      1200 Ω    -> 1.2 kΩ
    """
    if value is None:
        return f"? {unit}".strip()
    try:
        v = float(value)
    except (TypeError, ValueError):
        return f"{value} {unit}".strip()

    if v == 0.0:
        return f"0 {unit}".strip()

    av = abs(v)
    scales = [
        (1e12, "T"),
        (1e9, "G"),
        (1e6, "M"),
        (1e3, "k"),
        (1.0, ""),
        (1e-3, "m"),
        (1e-6, "µ"),
        (1e-9, "n"),
        (1e-12, "p"),
        (1e-15, "f"),
    ]
    for s, p in scales:
        if av >= s:
            return f"{v / s:.{sig}g} {p}{unit}".strip()
    return f"{v:.{sig}g} {unit}".strip()


def fmt_plain(value, unit: str = "", sig: int = 8) -> str:
    """
    Plain numeric formatting (no SI prefix scaling), intended for the user's
    "show current in A" and "show voltage in mV" request.
    """
    if value is None:
        return f"? {unit}".strip()
    try:
        v = float(value)
    except (TypeError, ValueError):
        return f"{value} {unit}".strip()
    return f"{v:.{sig}g} {unit}".strip()


def fmt_ohms(r) -> str:
    return fmt_eng(r, "Ω")


def fmt_amps_eng(i) -> str:
    return fmt_eng(i, "A")


def fmt_volts_eng(v) -> str:
    return fmt_eng(v, "V")


def fmt_millivolts_plain_from_volts(v, sig: int = 8) -> str:
    if v is None:
        return "? mV"
    try:
        vv = float(v)
    except (TypeError, ValueError):
        return f"{v} mV"
    return f"{(vv * 1e3):.{sig}g} mV"


def _dedup_by_key(items, key_fn):
    seen = set()
    out = []
    for it in items or []:
        k = key_fn(it)
        if k in seen:
            continue
        seen.add(k)
        out.append(it)
    return out


def load_json(path):
    with open(path, "r") as f:
        data = json.load(f)

    nodes = {}
    for n in data.get("nodes", []):
        nid = n["id"]
        nodes[nid] = n

    edges = data.get("edges", [])
    tick_to_um = float(data.get("meta", {}).get("tick_to_um", 1.0))
    return nodes, edges, tick_to_um


def compute_connectivity(nodes, edges):
    """
    Connectivity definition:
    - metal, via, tsv, pkg, vsrc, isrc all create undirected connections *between real nodes*
      when both endpoints exist in `nodes`.
    - vsrc/isrc may connect a real node to an external endpoint that is NOT in `nodes`;
      in that case, we still want to:
        * count node degree
        * mark the node as vsrc-/isrc-connected
        * (for vsrc) mark its component as powered

    Returns:
      comp_of_node: node_id -> component_root_id
      components: root_id -> list[node_id]
      powered_roots: set[root_id]
      degree: node_id -> degree across all edges that touch this node (even if the other endpoint is external)
      vsrc_nodes: set[node_id]
      isrc_nodes: set[node_id]
      vsrc_edge_ids_by_node: node_id -> list[str edge_id]
      isrc_edge_ids_by_node: node_id -> list[str edge_id]
      tsv_nodes: set[node_id]
      tsv_edge_ids_by_node: node_id -> list[str edge_id]

      NEW:
      vsrc_edges_by_node: node_id -> list[dict{id,subtype,n1,n2,v}]
      isrc_edges_by_node: node_id -> list[dict{id,subtype,n1,n2,i}]
    """
    dsu = DSU(nodes.keys())
    degree = defaultdict(int)

    vsrc_pkg_nodes = set()
    vsrc_nodes = set()
    isrc_nodes = set()
    tsv_nodes = set()

    vsrc_edge_ids_by_node = defaultdict(list)
    isrc_edge_ids_by_node = defaultdict(list)
    tsv_edge_ids_by_node = defaultdict(list)

    # NEW: keep value-bearing info (so hover can show V/I)
    vsrc_edges_by_node = defaultdict(list)
    isrc_edges_by_node = defaultdict(list)

    def eid_str(e):
        eid = e.get("id")
        return str(eid) if eid is not None else "(no-id)"

    for e in edges:
        n1 = e.get("n1")
        n2 = e.get("n2")

        # Degree should count any edge incident to a real node (even if the other endpoint is external)
        if n1 in nodes:
            degree[n1] += 1
        if n2 in nodes:
            degree[n2] += 1

        # Connectivity only unions real nodes
        if n1 in nodes and n2 in nodes:
            dsu.union(n1, n2)

        et = e.get("type")
        if et in ("vsrc", "isrc", "tsv"):
            eid_s = eid_str(e)

            if et == "vsrc":
                info = {
                    "id": eid_s,
                    "subtype": e.get("subtype"),
                    "n1": n1,
                    "n2": n2,
                    "v": e.get("v"),
                }
                for nid in (n1, n2):
                    if nid == "GND":
                        continue
                    vsrc_pkg_nodes.add(nid)
                    vsrc_edge_ids_by_node[nid].append(eid_s)
                    vsrc_edges_by_node[nid].append(info)

            elif et == "isrc":
                info = {
                    "id": eid_s,
                    "subtype": e.get("subtype"),
                    "n1": n1,
                    "n2": n2,
                    "i": e.get("i"),
                }
                for nid in (n1, n2):
                    if nid == "GND":
                        continue
                    isrc_nodes.add(nid)
                    isrc_edge_ids_by_node[nid].append(eid_s)
                    isrc_edges_by_node[nid].append(info)

            else:  # tsv
                for nid in (n1, n2):
                    if nid == "GND":
                        continue
                    tsv_nodes.add(nid)
                    tsv_edge_ids_by_node[nid].append(eid_s)

    # Second pass:
    # include the pdn node that connects to the pkg voltage node,
    # and propagate the vsrc info one hop so PDN nodes can show V values on hover.
    for e in edges:
        n1 = e.get("n1")
        n2 = e.get("n2")

        n1_is_pkg = n1 in vsrc_pkg_nodes
        n2_is_pkg = n2 in vsrc_pkg_nodes
        if not (n1_is_pkg or n2_is_pkg):
            continue

        # mark the endpoints as vsrc-connected (existing behavior)
        if n1 != "GND":
            vsrc_nodes.add(n1)
        if n2 != "GND":
            vsrc_nodes.add(n2)

        # NEW: propagate vsrc edge info from the pkg-node side to the other endpoint
        if n1_is_pkg and n2 != "GND":
            vsrc_edge_ids_by_node[n2].extend(vsrc_edge_ids_by_node.get(n1, []))
            vsrc_edges_by_node[n2].extend(vsrc_edges_by_node.get(n1, []))
        if n2_is_pkg and n1 != "GND":
            vsrc_edge_ids_by_node[n1].extend(vsrc_edge_ids_by_node.get(n2, []))
            vsrc_edges_by_node[n1].extend(vsrc_edges_by_node.get(n2, []))

    comp_of_node = {nid: dsu.find(nid) for nid in nodes.keys()}
    components = defaultdict(list)
    for nid, root in comp_of_node.items():
        components[root].append(nid)

    powered_roots = {comp_of_node[nid] for nid in vsrc_nodes if nid in comp_of_node}

    return (
        comp_of_node,
        components,
        powered_roots,
        degree,
        vsrc_nodes,
        isrc_nodes,
        vsrc_edge_ids_by_node,
        isrc_edge_ids_by_node,
        tsv_nodes,
        tsv_edge_ids_by_node,
        vsrc_edges_by_node,
        isrc_edges_by_node,
    )


def component_summary(nodes, components, powered_roots):
    rows = []
    for root, nids in components.items():
        layers = sorted({nodes[n].get("layer") for n in nids})
        xs = [nodes[n]["x_um"] for n in nids if nodes[n].get("x_um") is not None]
        ys = [nodes[n]["y_um"] for n in nids if nodes[n].get("y_um") is not None]
        bbox = None
        if xs and ys:
            bbox = (min(xs), min(ys), max(xs), max(ys))
        rows.append(
            {
                "root": root,
                "nodes": len(nids),
                "powered": (root in powered_roots),
                "layers": layers,
                "bbox": bbox,
            }
        )
    rows.sort(key=lambda r: r["nodes"], reverse=True)
    return rows


def _short_list(items, max_items=8):
    if not items:
        return "[]"
    s = [str(x) for x in items]
    if len(s) <= max_items:
        return "[" + ", ".join(s) + "]"
    return "[" + ", ".join(s[:max_items]) + f", ... (+{len(s) - max_items} more)]"


def make_layer_figure(
    nodes,
    edges,
    comp_of_node,
    powered_roots,
    degree,
    vsrc_nodes,
    isrc_nodes,
    vsrc_edge_ids_by_node,
    isrc_edge_ids_by_node,
    tsv_nodes,
    tsv_edge_ids_by_node,
    vsrc_edges_by_node,  # NEW
    isrc_edges_by_node,  # NEW
    layer: str,
    net: str,
    only_isolated: bool = False,
    focus_component_root: str | None = None,
    show_background: bool = True,
):
    # Collect nodes on this layer/net
    layer_nodes = {nid for nid, n in nodes.items() if n.get("layer") == layer and n.get("net") == net}

    # Determine focus set
    if focus_component_root is not None:
        focus_nodes = {nid for nid in layer_nodes if comp_of_node[nid] == focus_component_root}
    elif only_isolated:
        focus_nodes = {nid for nid in layer_nodes if comp_of_node[nid] not in powered_roots}
    else:
        focus_nodes = set(layer_nodes)

    # Positions
    pos = {}
    for nid in layer_nodes:
        n = nodes[nid]
        x = n.get("x_um")
        y = n.get("y_um")
        if x is None or y is None:
            continue
        pos[nid] = (float(x), float(y))

    def node_tags_str(nid):
        tags = []
        if nid in vsrc_nodes:
            tags.append("vsrc")
        if nid in isrc_nodes:
            tags.append("isrc")
        if nid in tsv_nodes:
            tags.append("tsv")
        return ",".join(tags) if tags else "-"

    # Helper to build batched line segments for edges on this layer
    def batched_lines(edge_filter_fn):
        xs, ys = [], []
        for e in edges:
            if not edge_filter_fn(e):
                continue
            n1, n2 = e.get("n1"), e.get("n2")
            if n1 not in pos or n2 not in pos:
                continue
            x1, y1 = pos[n1]
            x2, y2 = pos[n2]
            xs += [x1, x2, None]
            ys += [y1, y2, None]
        return xs, ys

    fig = go.Figure()

    # Metal legend groups (so hover-traces toggle with line-traces)
    METAL_BG_GROUP = f"metal_bg_{layer}_{net}"
    METAL_FOCUS_GROUP = f"metal_focus_{layer}_{net}"

    # Background metal edges on this layer
    if show_background:
        bg_xs, bg_ys = batched_lines(lambda e: e.get("type") == "metal" and e.get("layer") == layer)
        fig.add_trace(
            go.Scattergl(
                x=bg_xs,
                y=bg_ys,
                mode="lines",
                line=dict(color="rgba(150,150,150,0.25)", width=1),
                name=f"metal (all) {layer}",
                hoverinfo="skip",
                showlegend=True,
                legendgroup=METAL_BG_GROUP,
            )
        )

        # Background nodes
        bg_node_x, bg_node_y, bg_hover = [], [], []
        for nid in layer_nodes:
            if nid not in pos:
                continue
            x, y = pos[nid]
            n = nodes[nid]
            bg_node_x.append(x)
            bg_node_y.append(y)
            root = comp_of_node[nid]
            bg_hover.append(
                f"node {nid}<br>"
                f"layer={n.get('layer')} net={n.get('net')} net_id={n.get('net_id')}<br>"
                f"component={root} powered={root in powered_roots}<br>"
                f"degree={degree.get(nid,0)} tags={node_tags_str(nid)}"
            )

        fig.add_trace(
            go.Scattergl(
                x=bg_node_x,
                y=bg_node_y,
                mode="markers",
                marker=dict(size=3, color="rgba(120,120,120,0.35)"),
                name="nodes (all)",
                text=bg_hover,
                hoverinfo="text",
                showlegend=True,
            )
        )

    # Highlight focus metal edges (subset)
    hl_xs, hl_ys = batched_lines(
        lambda e: (
            e.get("type") == "metal"
            and e.get("layer") == layer
            and e.get("n1") in focus_nodes
            and e.get("n2") in focus_nodes
        )
    )
    have_focus_metal = bool(hl_xs)
    if have_focus_metal:
        fig.add_trace(
            go.Scattergl(
                x=hl_xs,
                y=hl_ys,
                mode="lines",
                line=dict(color="rgba(220,50,50,0.95)", width=2),
                name="metal (focus)",
                hoverinfo="skip",
                showlegend=True,
                legendgroup=METAL_FOCUS_GROUP,
            )
        )

    # --- Hoverable points on metal edges to show stripe R ---
    #
    # We MUST tie these hover points to the visibility of the corresponding metal line trace(s),
    # otherwise you can hide stripes via legend and still get tooltips on invisible geometry.
    #
    EDGE_HOVER_SPACING_UM = 40.0
    EDGE_HOVER_MAX_POINTS_PER_EDGE = 6
    EDGE_HOVER_MARKER_SIZE = 12

    def sample_edge_hover_points(edge_filter_fn):
        hx, hy, ht = [], [], []

        for e in edges:
            if not edge_filter_fn(e):
                continue

            n1, n2 = e.get("n1"), e.get("n2")
            if n1 not in pos or n2 not in pos:
                continue

            x1, y1 = pos[n1]
            x2, y2 = pos[n2]

            length_um = math.hypot(x2 - x1, y2 - y1)
            r = e.get("r")
            r_str = fmt_ohms(r)

            net1 = nodes.get(n1, {}).get("net")
            net2 = nodes.get(n2, {}).get("net")
            net_str = net1 if net1 == net2 else f"{net1}/{net2}"

            hover_text = (
                f"metal {e.get('id','(no-id)')}<br>"
                f"layer={e.get('layer')} net={net_str}<br>"
                f"{n1} ↔ {n2}<br>"
                f"R={r_str}"
            )
            if length_um > 0:
                hover_text += f"<br>len={length_um:.3f} µm"
                try:
                    hover_text += f"<br>R/len={fmt_eng(float(r) / length_um, 'Ω/µm')}"
                except Exception:
                    pass

            # sample points along the segment (exclude endpoints to avoid fighting node hover)
            if length_um <= 0:
                npts = 1
            else:
                npts = int(length_um / EDGE_HOVER_SPACING_UM) + 1
                npts = max(1, min(EDGE_HOVER_MAX_POINTS_PER_EDGE, npts))

            for i in range(1, npts + 1):
                t = i / (npts + 1)
                hx.append(x1 + t * (x2 - x1))
                hy.append(y1 + t * (y2 - y1))
                ht.append(hover_text)

        return hx, hy, ht

    # Background metal hover points: only for edges actually drawn by background metal trace
    if show_background:
        metal_hx_bg, metal_hy_bg, metal_ht_bg = sample_edge_hover_points(
            lambda e: (e.get("type") == "metal" and e.get("layer") == layer)
        )
        if metal_hx_bg:
            fig.add_trace(
                go.Scattergl(
                    x=metal_hx_bg,
                    y=metal_hy_bg,
                    mode="markers",
                    marker=dict(
                        size=EDGE_HOVER_MARKER_SIZE,
                        color="rgba(0,0,0,0.001)",  # invisible but hoverable
                    ),
                    text=metal_ht_bg,
                    hovertemplate="%{text}<extra></extra>",
                    name="metal hover (bg)",
                    showlegend=False,
                    legendgroup=METAL_BG_GROUP,
                )
            )

    # Focus metal hover points: only for edges actually drawn by focus metal trace
    if have_focus_metal:
        metal_hx_f, metal_hy_f, metal_ht_f = sample_edge_hover_points(
            lambda e: (
                e.get("type") == "metal"
                and e.get("layer") == layer
                and e.get("n1") in focus_nodes
                and e.get("n2") in focus_nodes
            )
        )
        if metal_hx_f:
            fig.add_trace(
                go.Scattergl(
                    x=metal_hx_f,
                    y=metal_hy_f,
                    mode="markers",
                    marker=dict(
                        size=EDGE_HOVER_MARKER_SIZE,
                        color="rgba(0,0,0,0.001)",  # invisible but hoverable
                    ),
                    text=metal_ht_f,
                    hovertemplate="%{text}<extra></extra>",
                    name="metal hover (focus)",
                    showlegend=False,
                    legendgroup=METAL_FOCUS_GROUP,
                )
            )

    # Highlight focus nodes (isolated vs powered component)
    fx, fy, fhover, fcolor = [], [], [], []
    for nid in focus_nodes:
        if nid not in pos:
            continue
        x, y = pos[nid]
        n = nodes[nid]
        root = comp_of_node[nid]
        is_isolated = root not in powered_roots
        fx.append(x)
        fy.append(y)
        fhover.append(
            f"node {nid}<br>"
            f"layer={n.get('layer')} net={n.get('net')}<br>"
            f"component={root} isolated={is_isolated}<br>"
            f"degree={degree.get(nid,0)} tags={node_tags_str(nid)}"
        )
        fcolor.append(0 if is_isolated else 1)

    if fx:
        colors = ["rgba(220,50,50,0.95)", "rgba(60,120,255,0.85)"]  # isolated, powered
        fig.add_trace(
            go.Scattergl(
                x=fx,
                y=fy,
                mode="markers",
                marker=dict(size=6, color=[colors[c] for c in fcolor]),
                name="nodes (focus)",
                text=fhover,
                hoverinfo="text",
                showlegend=True,
            )
        )

    # Via endpoints touching this layer
    via_x, via_y, via_hover, via_symbol = [], [], [], []
    for e in edges:
        if e.get("type") != "via":
            continue
        n1, n2 = e.get("n1"), e.get("n2")
        if n1 not in nodes or n2 not in nodes:
            continue
        l1 = nodes[n1].get("layer")
        l2 = nodes[n2].get("layer")

        if l1 == layer and n1 in pos:
            other_layer = l2
            r_this = layer_rank(layer)
            r_other = layer_rank(other_layer)
            sym = "triangle-up" if r_other > r_this else "triangle-down"
            x, y = pos[n1]
            via_x.append(x)
            via_y.append(y)
            via_symbol.append(sym)
            via_hover.append(
                f"via {e.get('id')}<br>{n1}@{l1} ↔ {n2}@{l2}<br>R={fmt_ohms(e.get('r'))}"
            )

        if l2 == layer and n2 in pos:
            other_layer = l1
            r_this = layer_rank(layer)
            r_other = layer_rank(other_layer)
            sym = "triangle-up" if r_other > r_this else "triangle-down"
            x, y = pos[n2]
            via_x.append(x)
            via_y.append(y)
            via_symbol.append(sym)
            via_hover.append(
                f"via {e.get('id')}<br>{n2}@{l2} ↔ {n1}@{l1}<br>R={fmt_ohms(e.get('r'))}"
            )

    if via_x:
        fig.add_trace(
            go.Scattergl(
                x=via_x,
                y=via_y,
                mode="markers",
                marker=dict(
                    size=9,
                    color="rgba(0,160,255,0.95)",
                    symbol=via_symbol,
                    line=dict(width=1, color="rgba(0,0,0,0.4)"),
                ),
                name="vias (endpoints)",
                text=via_hover,
                hoverinfo="text",
                showlegend=True,
            )
        )

    # TSV endpoints touching this layer (similar to vias, but distinct styling)
    tsv_x, tsv_y, tsv_hover, tsv_symbol = [], [], [], []
    for e in edges:
        if e.get("type") != "tsv":
            continue
        n1, n2 = e.get("n1"), e.get("n2")
        if n1 not in nodes or n2 not in nodes:
            continue
        l1 = nodes[n1].get("layer")
        l2 = nodes[n2].get("layer")

        r_this = layer_rank(layer)

        if l1 == layer and n1 in pos:
            r_other = layer_rank(l2)
            sym = "triangle-up-open" if r_other > r_this else "triangle-down-open"
            x, y = pos[n1]
            tsv_x.append(x)
            tsv_y.append(y)
            tsv_symbol.append(sym)
            tsv_hover.append(
                f"tsv {e.get('id')}<br>{n1}@{l1} ↔ {n2}@{l2}<br>R={fmt_ohms(e.get('r'))}"
            )

        if l2 == layer and n2 in pos:
            r_other = layer_rank(l1)
            sym = "triangle-up-open" if r_other > r_this else "triangle-down-open"
            x, y = pos[n2]
            tsv_x.append(x)
            tsv_y.append(y)
            tsv_symbol.append(sym)
            tsv_hover.append(
                f"tsv {e.get('id')}<br>{n2}@{l2} ↔ {n1}@{l1}<br>R={fmt_ohms(e.get('r'))}"
            )

    if tsv_x:
        fig.add_trace(
            go.Scattergl(
                x=tsv_x,
                y=tsv_y,
                mode="markers",
                marker=dict(
                    size=12,
                    color="rgba(0,200,120,0.95)",
                    symbol=tsv_symbol,
                    line=dict(width=2, color="rgba(0,200,120,0.95)"),
                ),
                name="tsvs (endpoints)",
                text=tsv_hover,
                hoverinfo="text",
                showlegend=True,
            )
        )

    # --- emphasize nodes connected to vsrc/isrc/tsv ---
    VSRC_COLOR = "rgba(180,0,255,0.95)"
    ISRC_COLOR = "rgba(255,140,0,0.95)"
    BOTH_COLOR = "rgba(0,0,0,0.95)"

    VSRC_STYLE = dict(
        size=16,
        symbol="circle-open",
        color=VSRC_COLOR,
        line=dict(width=3, color=VSRC_COLOR),
    )
    ISRC_STYLE = dict(
        size=16,
        symbol="square-open",
        color=ISRC_COLOR,
        line=dict(width=3, color=ISRC_COLOR),
    )
    BOTH_STYLE = dict(
        size=18,
        symbol="diamond-open",
        color=BOTH_COLOR,
        line=dict(width=3, color=BOTH_COLOR),
    )

    VSRC_HOVERLABEL = dict(
        bgcolor="rgba(180,0,255,0.10)",
        bordercolor=VSRC_COLOR,
        font=dict(color=VSRC_COLOR),
    )
    ISRC_HOVERLABEL = dict(
        bgcolor="rgba(255,140,0,0.10)",
        bordercolor=ISRC_COLOR,
        font=dict(color=ISRC_COLOR),
    )
    BOTH_HOVERLABEL = dict(
        bgcolor="rgba(0,0,0,0.08)",
        bordercolor=BOTH_COLOR,
        font=dict(color=BOTH_COLOR),
    )

    vsrc_on_layer = (layer_nodes & vsrc_nodes) - isrc_nodes
    isrc_on_layer = (layer_nodes & isrc_nodes) - vsrc_nodes
    both_on_layer = layer_nodes & vsrc_nodes & isrc_nodes

    def _src_value_lines_for_node(nid: str, include_vsrc: bool, include_isrc: bool, max_items: int = 6):
        lines = []

        if include_vsrc:
            v_infos = _dedup_by_key(vsrc_edges_by_node.get(nid, []), lambda d: d.get("id"))
            for s in v_infos[:max_items]:
                v = s.get("v")
                lines.append(
                    "• "
                    + f"VSRC {s.get('id','(no-id)')} ({s.get('subtype','?')}): "
                    + f"V={fmt_millivolts_plain_from_volts(v)} ({fmt_volts_eng(v)}) "
                    + f"[{s.get('n1','?')} → {s.get('n2','?')}]"
                )
            if len(v_infos) > max_items:
                lines.append(f"• ... (+{len(v_infos) - max_items} more VSRC)")

        if include_isrc:
            i_infos = _dedup_by_key(isrc_edges_by_node.get(nid, []), lambda d: d.get("id"))
            for s in i_infos[:max_items]:
                i = s.get("i")
                lines.append(
                    "• "
                    + f"ISRC {s.get('id','(no-id)')} ({s.get('subtype','?')}): "
                    + f"I={fmt_plain(i, 'A')} ({fmt_amps_eng(i)}) "
                    + f"[{s.get('n1','?')} → {s.get('n2','?')}]"
                )
            if len(i_infos) > max_items:
                lines.append(f"• ... (+{len(i_infos) - max_items} more ISRC)")

        return lines

    def build_tag_trace(nids, label, include_vsrc: bool, include_isrc: bool):
        xs, ys, hovers = [], [], []
        for nid in nids:
            if nid not in pos:
                continue
            x, y = pos[nid]
            n = nodes[nid]
            root = comp_of_node[nid]

            v_ids = _short_list(vsrc_edge_ids_by_node.get(nid, []))
            i_ids = _short_list(isrc_edge_ids_by_node.get(nid, []))
            t_ids = _short_list(tsv_edge_ids_by_node.get(nid, []))

            src_lines = _src_value_lines_for_node(nid, include_vsrc=include_vsrc, include_isrc=include_isrc)
            src_block = ""
            if src_lines:
                src_block = "<br><b>source values</b><br>" + "<br>".join(src_lines)

            xs.append(x)
            ys.append(y)
            hovers.append(
                f"{label} node {nid}<br>"
                f"layer={n.get('layer')} net={n.get('net')}<br>"
                f"tsv_edges={t_ids}<br>"
                f"vsrc_edges={v_ids}<br>"
                f"isrc_edges={i_ids}"
                f"{src_block}<br>"
                f"component={root} powered={root in powered_roots}<br>"
                f"degree={degree.get(nid,0)}"
            )
        return xs, ys, hovers

    if vsrc_on_layer:
        xs, ys, hovers = build_tag_trace(vsrc_on_layer, "VSRC-connected", include_vsrc=True, include_isrc=False)
        if xs:
            fig.add_trace(
                go.Scattergl(
                    x=xs,
                    y=ys,
                    mode="markers",
                    marker=VSRC_STYLE,
                    name="nodes w/ vsrc",
                    text=hovers,
                    hovertemplate="%{text}<extra></extra>",
                    hoverlabel=VSRC_HOVERLABEL,
                    showlegend=True,
                )
            )

    if isrc_on_layer:
        xs, ys, hovers = build_tag_trace(isrc_on_layer, "ISRC-connected", include_vsrc=False, include_isrc=True)
        if xs:
            fig.add_trace(
                go.Scattergl(
                    x=xs,
                    y=ys,
                    mode="markers",
                    marker=ISRC_STYLE,
                    name="nodes w/ isrc",
                    text=hovers,
                    hovertemplate="%{text}<extra></extra>",
                    hoverlabel=ISRC_HOVERLABEL,
                    showlegend=True,
                )
            )

    if both_on_layer:
        xs, ys, hovers = build_tag_trace(both_on_layer, "VSRC+ISRC-connected", include_vsrc=True, include_isrc=True)
        if xs:
            fig.add_trace(
                go.Scattergl(
                    x=xs,
                    y=ys,
                    mode="markers",
                    marker=BOTH_STYLE,
                    name="nodes w/ vsrc+isrc",
                    text=hovers,
                    hovertemplate="%{text}<extra></extra>",
                    hoverlabel=BOTH_HOVERLABEL,
                    showlegend=True,
                )
            )

    # Open circuits (degree 0) on this layer
    open_x, open_y, open_hover = [], [], []
    for nid in layer_nodes:
        if degree.get(nid, 0) != 0:
            continue
        if nid not in pos:
            continue
        x, y = pos[nid]
        open_x.append(x)
        open_y.append(y)
        open_hover.append(
            f"OPEN node {nid}<br>layer={layer} net={nodes[nid].get('net')} tags={node_tags_str(nid)}"
        )

    if open_x:
        fig.add_trace(
            go.Scattergl(
                x=open_x,
                y=open_y,
                mode="markers",
                marker=dict(size=14, color="rgba(255,0,0,0.95)", symbol="x"),
                name="open-circuit nodes (degree=0)",
                text=open_hover,
                hoverinfo="text",
                showlegend=True,
            )
        )

    # Layout tweaks
    fig.update_layout(
        title=(
            f"PDN connectivity — layer {layer}"
            + (" (isolated only)" if only_isolated else "")
            + (f" (focus component {focus_component_root})" if focus_component_root else "")
        ),
        template="plotly_white",
        legend=dict(
            orientation="h",
            groupclick="togglegroup",  # IMPORTANT: makes legendgroup hide/show the hover-trace too
        ),
        margin=dict(l=10, r=10, t=50, b=10),
        hovermode="closest",
        hoverdistance=30,
    )
    fig.update_yaxes(scaleanchor="x", scaleratio=1)

    # Auto-zoom to focus set if requested
    if focus_component_root is not None or only_isolated:
        xs = [pos[n][0] for n in focus_nodes if n in pos]
        ys = [pos[n][1] for n in focus_nodes if n in pos]
        if xs and ys:
            pad_x = (max(xs) - min(xs)) * 0.05 + 1e-6
            pad_y = (max(ys) - min(ys)) * 0.05 + 1e-6
            fig.update_xaxes(range=[min(xs) - pad_x, max(xs) + pad_x])
            fig.update_yaxes(range=[min(ys) - pad_y, max(ys) + pad_y])

    return fig


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("json", help="JSON exported from C++")
    ap.add_argument("--layer", required=True, help="Layer name, e.g. M4")
    ap.add_argument("--net", required=True, help="Net name, e.g. VDD")
    ap.add_argument("--out", default=None, help="Output HTML path")
    ap.add_argument(
        "--only-isolated",
        action="store_true",
        help="Show only isolated components (plus vias on that layer)",
    )
    ap.add_argument(
        "--focus-component",
        default=None,
        help="Component root id to focus (printed by --list-components)",
    )
    ap.add_argument(
        "--no-background",
        action="store_true",
        help="Do not draw faint full-network background",
    )
    ap.add_argument(
        "--list-components",
        action="store_true",
        help="Print components summary and exit",
    )
    args = ap.parse_args(argv)

    nodes, edges, _tick_to_um = load_json(args.json)
    (
        comp_of_node,
        components,
        powered_roots,
        degree,
        vsrc_nodes,
        isrc_nodes,
        vsrc_edge_ids_by_node,
        isrc_edge_ids_by_node,
        tsv_nodes,
        tsv_edge_ids_by_node,
        vsrc_edges_by_node,
        isrc_edges_by_node,
    ) = compute_connectivity(nodes, edges)

    if args.list_components:
        rows = component_summary(nodes, components, powered_roots)
        print("component_root  nodes  powered  layers  bbox(minx,miny,maxx,maxy)")
        for r in rows[:200]:
            print(
                f"{r['root']}  {r['nodes']}  {int(r['powered'])}  "
                f"{','.join([x for x in r['layers'] if x])}  {r['bbox']}"
            )
        return

    out = args.out
    if out is None:
        suffix = "isolated" if args.only_isolated else "all"
        out = f"pdn_{args.layer}_{suffix}.html"

    fig = make_layer_figure(
        nodes,
        edges,
        comp_of_node,
        powered_roots,
        degree,
        vsrc_nodes,
        isrc_nodes,
        vsrc_edge_ids_by_node,
        isrc_edge_ids_by_node,
        tsv_nodes,
        tsv_edge_ids_by_node,
        vsrc_edges_by_node,
        isrc_edges_by_node,
        layer=args.layer,
        net=args.net,
        only_isolated=args.only_isolated,
        focus_component_root=args.focus_component,
        show_background=not args.no_background,
    )

    # If you prefer a standalone HTML:
    # fig.write_html(out, include_plotlyjs="cdn")
    fig.show()


if __name__ == "__main__":
    # main()
    main([
        "../viz_output/viz.json",
        "--layer",
        "met4",
        "--net",
        "VSS"
    ])