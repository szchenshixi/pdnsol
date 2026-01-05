#!/usr/bin/env python3
import argparse
import json
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
      powered_roots: set[root_id] (contains at least one vsrc-connected node)
      degree: node_id -> degree across all edges that touch this node (even if the other endpoint is external)
      vsrc_nodes: set[node_id]
      isrc_nodes: set[node_id]
      vsrc_edge_ids_by_node: node_id -> list[str edge_id]
      isrc_edge_ids_by_node: node_id -> list[str edge_id]
      tsv_nodes: set[node_id]
      tsv_edge_ids_by_node: node_id -> list[str edge_id]
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
            eid = e.get("id")
            eid_s = str(eid) if eid is not None else "(no-id)"
            for nid in (n1, n2):
                if et == "vsrc":
                    vsrc_pkg_nodes.add(nid)
                    vsrc_edge_ids_by_node[nid].append(eid_s)
                elif et == "isrc":
                    isrc_nodes.add(nid)
                    isrc_edge_ids_by_node[nid].append(eid_s)
                else:  # tsv
                    tsv_nodes.add(nid)
                    tsv_edge_ids_by_node[nid].append(eid_s)

    # Second pass to include the pdn node that connects to the pkg voltage node
    for e in edges:
        n1 = e.get("n1")
        n2 = e.get("n2")

        if n1 in vsrc_pkg_nodes or n2 in vsrc_pkg_nodes:
            vsrc_nodes.add(n1)
            vsrc_nodes.add(n2)

    comp_of_node = {nid: dsu.find(nid) for nid in nodes.keys()}
    components = defaultdict(list)
    for nid, root in comp_of_node.items():
        components[root].append(nid)

    # Powered components: any component that contains at least one vsrc-connected node
    powered_roots = {
        comp_of_node[nid]
        for nid in vsrc_nodes if nid in comp_of_node
    }

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
    )


def component_summary(nodes, components, powered_roots):
    """
    Build simple summary list sorted by node count desc.
    """
    rows = []
    for root, nids in components.items():
        layers = sorted({nodes[n].get("layer") for n in nids})
        xs = [
            nodes[n]["x_um"] for n in nids if nodes[n].get("x_um") is not None
        ]
        ys = [
            nodes[n]["y_um"] for n in nids if nodes[n].get("y_um") is not None
        ]
        bbox = None
        if xs and ys:
            bbox = (min(xs), min(ys), max(xs), max(ys))
        rows.append({
            "root": root,
            "nodes": len(nids),
            "powered": (root in powered_roots),
            "layers": layers,
            "bbox": bbox,
        })
    rows.sort(key=lambda r: r["nodes"], reverse=True)
    return rows


def _short_list(items, max_items=8):
    """
    Render a list like [a, b, c] but truncate if it's long.
    """
    if not items:
        return "[]"
    s = [str(x) for x in items]
    if len(s) <= max_items:
        return "[" + ", ".join(s) + "]"
    return "[" + ", ".join(
        s[:max_items]) + f", ... (+{len(s) - max_items} more)]"


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
    layer: str,
    net: str,
    only_isolated: bool = False,
    focus_component_root: str | None = None,
    show_background: bool = True,
):
    # Collect nodes on this layer/net
    layer_nodes = {
        nid
        for nid, n in nodes.items()
        if n.get("layer") == layer and n.get("net") == net
    }

    # Determine focus set
    if focus_component_root is not None:
        focus_nodes = {
            nid
            for nid in layer_nodes if comp_of_node[nid] == focus_component_root
        }
    elif only_isolated:
        focus_nodes = {
            nid
            for nid in layer_nodes if comp_of_node[nid] not in powered_roots
        }
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

    # Background metal edges on this layer
    if show_background:
        bg_xs, bg_ys = batched_lines(
            lambda e: e.get("type") == "metal" and e.get("layer") == layer)
        fig.add_trace(
            go.Scattergl(
                x=bg_xs,
                y=bg_ys,
                mode="lines",
                line=dict(color="rgba(150,150,150,0.25)", width=1),
                name=f"metal (all) {layer}",
                hoverinfo="skip",
                showlegend=True,
            ))

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
                f"degree={degree.get(nid,0)} tags={node_tags_str(nid)}")

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
            ))

    # Highlight focus metal edges (subset)
    hl_xs, hl_ys = batched_lines(
        lambda e: (e.get("type") == "metal" and e.get("layer") == layer and e.
                   get("n1") in focus_nodes and e.get("n2") in focus_nodes))
    if hl_xs:
        fig.add_trace(
            go.Scattergl(
                x=hl_xs,
                y=hl_ys,
                mode="lines",
                line=dict(color="rgba(220,50,50,0.95)", width=2),
                name="metal (focus)",
                hoverinfo="skip",
                showlegend=True,
            ))

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
        fhover.append(f"node {nid}<br>"
                      f"layer={n.get('layer')} net={n.get('net')}<br>"
                      f"component={root} isolated={is_isolated}<br>"
                      f"degree={degree.get(nid,0)} tags={node_tags_str(nid)}")
        fcolor.append(0 if is_isolated else 1)

    if fx:
        colors = ["rgba(220,50,50,0.95)",
                  "rgba(60,120,255,0.85)"]  # isolated, powered
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
            ))

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

        # If either endpoint is on this layer, draw marker at that endpoint (only)
        if l1 == layer and n1 in pos:
            other_layer = l2
            r_this = layer_rank(layer)
            r_other = layer_rank(other_layer)
            sym = "triangle-up" if r_other > r_this else "triangle-down"
            x, y = pos[n1]
            via_x.append(x)
            via_y.append(y)
            via_symbol.append(sym)
            via_hover.append(f"via {e.get('id')}<br>{n1}@{l1} ↔ {n2}@{l2}")

        if l2 == layer and n2 in pos:
            other_layer = l1
            r_this = layer_rank(layer)
            r_other = layer_rank(other_layer)
            sym = "triangle-up" if r_other > r_this else "triangle-down"
            x, y = pos[n2]
            via_x.append(x)
            via_y.append(y)
            via_symbol.append(sym)
            via_hover.append(f"via {e.get('id')}<br>{n2}@{l2} ↔ {n1}@{l1}")

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
            ))

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

        # Endpoint n1 on this layer
        if l1 == layer and n1 in pos:
            r_other = layer_rank(l2)
            sym = "triangle-up-open" if r_other > r_this else "triangle-down-open"
            x, y = pos[n1]
            tsv_x.append(x)
            tsv_y.append(y)
            tsv_symbol.append(sym)
            tsv_hover.append(
                f"tsv {e.get('id')}<br>{n1}@{l1} ↔ {n2}@{l2}<br>r={e.get('r')}"
            )

        # Endpoint n2 on this layer
        if l2 == layer and n2 in pos:
            r_other = layer_rank(l1)
            sym = "triangle-up-open" if r_other > r_this else "triangle-down-open"
            x, y = pos[n2]
            tsv_x.append(x)
            tsv_y.append(y)
            tsv_symbol.append(sym)
            tsv_hover.append(
                f"tsv {e.get('id')}<br>{n2}@{l2} ↔ {n1}@{l1}<br>r={e.get('r')}"
            )

    if tsv_x:
        fig.add_trace(
            go.Scattergl(
                x=tsv_x,
                y=tsv_y,
                mode="markers",
                marker=dict(
                    size=11,
                    color="rgba(0,200,120,0.95)",
                    symbol=tsv_symbol,
                    line=dict(width=2, color="rgba(0,200,120,0.95)"),
                ),
                name="tsvs (endpoints)",
                text=tsv_hover,
                hoverinfo="text",
                showlegend=True,
            ))

    # --- emphasize nodes connected to vsrc/isrc ---
    VSRC_STYLE = dict(
        size=16,
        symbol="circle-open",
        color="rgba(180,0,255,0.95)",
        line=dict(width=3, color="rgba(180,0,255,0.95)"),
    )
    ISRC_STYLE = dict(
        size=16,
        symbol="square-open",
        color="rgba(255,140,0,0.95)",
        line=dict(width=3, color="rgba(255,140,0,0.95)"),
    )
    BOTH_STYLE = dict(
        size=18,
        symbol="diamond-open",
        color="rgba(0,0,0,0.95)",
        line=dict(width=3, color="rgba(0,0,0,0.95)"),
    )
    TSV_STYLE = dict(
        size=16,
        symbol="hexagon-open",
        color="rgba(0,200,120,0.95)",
        line=dict(width=3, color="rgba(0,200,120,0.95)"),
    )

    vsrc_on_layer = (layer_nodes & vsrc_nodes) - isrc_nodes
    isrc_on_layer = (layer_nodes & isrc_nodes) - vsrc_nodes
    both_on_layer = layer_nodes & vsrc_nodes & isrc_nodes

    def build_tag_trace(nids, label):
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
            xs.append(x)
            ys.append(y)
            hovers.append(
                f"{label} node {nid}<br>"
                f"layer={n.get('layer')} net={n.get('net')}<br>"
                f"tsv_edges={t_ids}<br>"
                f"vsrc_edges={v_ids}<br>"
                f"isrc_edges={i_ids}<br>"
                f"component={root} powered={root in powered_roots}<br>"
                f"degree={degree.get(nid,0)}")
        return xs, ys, hovers

    # Add in a stable order so legend is predictable
    if vsrc_on_layer:
        xs, ys, hovers = build_tag_trace(vsrc_on_layer, "VSRC-connected")
        if xs:
            fig.add_trace(
                go.Scattergl(
                    x=xs,
                    y=ys,
                    mode="markers",
                    marker=VSRC_STYLE,
                    name="nodes w/ vsrc",
                    text=hovers,
                    hoverinfo="text",
                    showlegend=True,
                ))

    if isrc_on_layer:
        xs, ys, hovers = build_tag_trace(isrc_on_layer, "ISRC-connected")
        if xs:
            fig.add_trace(
                go.Scattergl(
                    x=xs,
                    y=ys,
                    mode="markers",
                    marker=ISRC_STYLE,
                    name="nodes w/ isrc",
                    text=hovers,
                    hoverinfo="text",
                    showlegend=True,
                ))

    if both_on_layer:
        xs, ys, hovers = build_tag_trace(both_on_layer, "VSRC+ISRC-connected")
        if xs:
            fig.add_trace(
                go.Scattergl(
                    x=xs,
                    y=ys,
                    mode="markers",
                    marker=BOTH_STYLE,
                    name="nodes w/ vsrc+isrc",
                    text=hovers,
                    hoverinfo="text",
                    showlegend=True,
                ))

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
            ))

    # Layout tweaks
    fig.update_layout(
        title=(f"PDN connectivity — layer {layer}" +
               (" (isolated only)" if only_isolated else "") +
               (f" (focus component {focus_component_root})"
                if focus_component_root else "")),
        template="plotly_white",
        legend=dict(orientation="h"),
        margin=dict(l=10, r=10, t=50, b=10),
        hovermode="closest",
    )
    fig.update_yaxes(scaleanchor="x",
                     scaleratio=1)  # keep geometry aspect ratio

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
    ) = compute_connectivity(nodes, edges)

    if args.list_components:
        rows = component_summary(nodes, components, powered_roots)
        print(
            "component_root  nodes  powered  layers  bbox(minx,miny,maxx,maxy)"
        )
        for r in rows[:200]:
            print(
                f"{r['root']}  {r['nodes']}  {int(r['powered'])}  {','.join([x for x in r['layers'] if x])}  {r['bbox']}"
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
        layer=args.layer,
        net=args.net,
        only_isolated=args.only_isolated,
        focus_component_root=args.focus_component,
        show_background=not args.no_background,
    )

    # fig.write_html(out, include_plotlyjs="cdn")
    fig.show()
    # print(f"Wrote {out}")


if __name__ == "__main__":
    # main()
    main([
        "../viz_output/viz.json",
        "--layer",
        "met4",
        "--net",
        "VDD",
        "--out",
        "met3_VSS_all.png",
    ])
