#!/usr/bin/env python3
import argparse
import json
import math
import re
from collections import defaultdict

import plotly.graph_objects as go
import plotly.express as px


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
    Replicates your sanitizer connectivity definition:
    - metal, via, pkg, vsrc, isrc all create undirected connections.
    Returns:
      comp_of_node: node_id -> component_root_id
      components: root_id -> list[node_id]
      powered_roots: set[root_id] (contains at least one vsrc edge)
      degree: node_id -> degree across all edge types
    """
    dsu = DSU(nodes.keys())
    degree = defaultdict(int)

    # Union all connections
    for e in edges:
        n1 = e.get("n1")
        n2 = e.get("n2")
        if n1 in nodes and n2 in nodes:
            dsu.union(n1, n2)
            degree[n1] += 1
            degree[n2] += 1

    comp_of_node = {nid: dsu.find(nid) for nid in nodes.keys()}
    components = defaultdict(list)
    for nid, root in comp_of_node.items():
        components[root].append(nid)

    # Powered components: any component that contains a Vsrc
    powered_roots = set()
    for e in edges:
        if e.get("type") != "vsrc":
            continue
        n1 = e.get("n1")
        if n1 in nodes:
            powered_roots.add(comp_of_node[n1])

    return comp_of_node, components, powered_roots, degree


def component_summary(nodes, components, powered_roots):
    """
    Build simple summary list sorted by node count desc.
    """
    rows = []
    for root, nids in components.items():
        layers = sorted({nodes[n]["layer"] for n in nids})
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
            "bbox": bbox
        })
    rows.sort(key=lambda r: r["nodes"], reverse=True)
    return rows


def make_layer_figure(nodes,
                      edges,
                      comp_of_node,
                      powered_roots,
                      degree,
                      layer: str,
                      net: str,
                      only_isolated: bool = False,
                      focus_component_root: str | None = None,
                      show_background: bool = True):
    # Collect nodes on this layer
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

    # Helper to build batched line segments for edges on this layer
    def batched_lines(edge_filter_fn):
        xs, ys, hover = [], [], []
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
            hover.append(f"{e.get('type')} {e.get('id')}<br>{n1} ↔ {n2}")
        return xs, ys

    fig = go.Figure()

    # Background metal edges on this layer
    if show_background:
        bg_xs, bg_ys = batched_lines(
            lambda e: e.get("type") == "metal" and e.get("layer") == layer)
        fig.add_trace(
            go.Scattergl(x=bg_xs,
                         y=bg_ys,
                         mode="lines",
                         line=dict(color="rgba(150,150,150,0.25)", width=1),
                         name=f"metal (all) {layer}",
                         hoverinfo="skip",
                         showlegend=True))

        # Background nodes
        bg_node_x = []
        bg_node_y = []
        bg_hover = []
        for nid in layer_nodes:
            if nid not in pos:
                continue
            x, y = pos[nid]
            n = nodes[nid]
            bg_node_x.append(x)
            bg_node_y.append(y)
            bg_hover.append(
                f"node {nid}<br>"
                f"layer={n.get('layer')} net={n.get('net')} net_id={n.get('net_id')}<br>"
                f"component={comp_of_node[nid]} powered={comp_of_node[nid] in powered_roots}<br>"
                f"degree={degree.get(nid,0)}")

        fig.add_trace(
            go.Scattergl(x=bg_node_x,
                         y=bg_node_y,
                         mode="markers",
                         marker=dict(size=3, color="rgba(120,120,120,0.35)"),
                         name="nodes (all)",
                         text=bg_hover,
                         hoverinfo="text",
                         showlegend=True))

    # Highlight focus metal edges (subset)
    hl_xs, hl_ys = batched_lines(
        lambda e: (e.get("type") == "metal" and e.get("layer") == layer and e.
                   get("n1") in focus_nodes and e.get("n2") in focus_nodes))
    if hl_xs:
        fig.add_trace(
            go.Scattergl(x=hl_xs,
                         y=hl_ys,
                         mode="lines",
                         line=dict(color="rgba(220,50,50,0.95)", width=2),
                         name="metal (focus)",
                         hoverinfo="skip",
                         showlegend=True))

    # Highlight focus nodes
    fx = []
    fy = []
    fhover = []
    fcolor = []
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
                      f"degree={degree.get(nid,0)}")
        fcolor.append(0 if is_isolated else 1)

    if fx:
        # Use a fixed mapping: isolated=red, powered=blue (when not only_isolated)
        colors = ["rgba(220,50,50,0.95)", "rgba(60,120,255,0.85)"]
        fig.add_trace(
            go.Scattergl(x=fx,
                         y=fy,
                         mode="markers",
                         marker=dict(size=6,
                                     color=[colors[c] for c in fcolor]),
                         name="nodes (focus)",
                         text=fhover,
                         hoverinfo="text",
                         showlegend=True))

    # Via endpoints touching this layer
    via_x = []
    via_y = []
    via_hover = []
    via_symbol = []

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
            go.Scattergl(x=via_x,
                         y=via_y,
                         mode="markers",
                         marker=dict(size=9,
                                     color="rgba(0,160,255,0.95)",
                                     symbol=via_symbol,
                                     line=dict(width=1,
                                               color="rgba(0,0,0,0.4)")),
                         name="vias (endpoints)",
                         text=via_hover,
                         hoverinfo="text",
                         showlegend=True))

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
            f"OPEN node {nid}<br>layer={layer} net={nodes[nid].get('net')}")

    if open_x:
        fig.add_trace(
            go.Scattergl(x=open_x,
                         y=open_y,
                         mode="markers",
                         marker=dict(size=14,
                                     color="rgba(255,0,0,0.95)",
                                     symbol="x"),
                         name="open-circuit nodes (degree=0)",
                         text=open_hover,
                         hoverinfo="text",
                         showlegend=True))

    # Layout tweaks
    fig.update_layout(
        title=f"PDN connectivity — layer {layer}" +
        (" (isolated only)" if only_isolated else "") +
        (f" (focus component {focus_component_root})"
         if focus_component_root else ""),
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
    ap.add_argument("--net", required=True, help="Net name, e.g. VSS")
    ap.add_argument("--out", default=None, help="Output HTML path")
    ap.add_argument(
        "--only-isolated",
        action="store_true",
        help="Show only isolated components (plus vias on that layer)")
    ap.add_argument(
        "--focus-component",
        default=None,
        help="Component root id to focus (printed by --list-components)")
    ap.add_argument("--no-background",
                    action="store_true",
                    help="Do not draw faint full-network background")
    ap.add_argument("--list-components",
                    action="store_true",
                    help="Print components summary and exit")
    args = ap.parse_args(argv)

    nodes, edges, _tick_to_um = load_json(args.json)
    comp_of_node, components, powered_roots, degree = compute_connectivity(
        nodes, edges)

    if args.list_components:
        rows = component_summary(nodes, components, powered_roots)
        print(
            "component_root  nodes  powered  layers  bbox(minx,miny,maxx,maxy)"
        )
        for r in rows[:200]:  # print top 200 by size
            print(
                f"{r['root']}  {r['nodes']}  {int(r['powered'])}  {','.join(r['layers'])}  {r['bbox']}"
            )
        return

    out = args.out
    if out is None:
        suffix = "isolated" if args.only_isolated else "all"
        out = f"pdn_{args.layer}_{args.net}_{suffix}.html"

    fig = make_layer_figure(nodes,
                            edges,
                            comp_of_node,
                            powered_roots,
                            degree,
                            layer=args.layer,
                            net=args.net,
                            only_isolated=args.only_isolated,
                            focus_component_root=args.focus_component,
                            show_background=not args.no_background)
    # fig.write_html(out, include_plotlyjs="cdn")
    # print(f"Wrote {out}")
    fig.show()


if __name__ == "__main__":
    # main()
    main([
        "viz_output/viz.json", "--layer", "met3", "--net", "VSS", "--out",
        "met3_VSS_all.html"
    ])
