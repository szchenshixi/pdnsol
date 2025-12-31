#!/usr/bin/env python3
"""
pdn_def_analyzer.py

Analyze PDN distribution patterns in a DEF file's SPECIALNETS section.

What it reports (per net-layer, e.g. VDD-met4):
  - STRIPE segments: width(s), direction(s), inferred pitch per direction,
    pitch consistency, and outliers.
  - FOLLOWPIN segments: treated the same as STRIPE (direction, pitch, outliers).
  - VIAs: detected as trailing tokens after coordinate(s), summarized by via type
    including inferred pitch in X/Y (when possible) and outliers.

Key fixes vs the flawed implementation:
  1) FOLLOWPIN is analyzed like STRIPE (direction + pitch + outliers).
  2) VIA handling is fixed: vias are detected from trailing via names like
     "... ( x y ) via3_1920x980" (no "VIA" keyword required).
"""

from __future__ import annotations

import argparse
import math
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple, Any

# -----------------------------
# Regexes
# -----------------------------

SPECIALNETS_START_RE = re.compile(r"^\s*SPECIALNETS\b", re.IGNORECASE)
SPECIALNETS_END_RE = re.compile(r"^\s*END\s+SPECIALNETS\b", re.IGNORECASE)
NET_START_RE = re.compile(r"^\s*-\s+(\S+)\b")  # "- VDD ..." => "VDD"
NET_END_RE = re.compile(r";\s*$")

# Routing clause starts (important: allows splitting "... viaName NEW met2 ...")
CLAUSE_START_RE = re.compile(r"(?:\+\s*)?(?:ROUTED|FIXED|COVER|NEW)\b",
                             re.IGNORECASE)

# Parse clause header: "NEW met4 1920 ..." or "+ ROUTED met3 0 ..."
CLAUSE_HEADER_RE = re.compile(
    r"^\s*(?:\+\s*)?(ROUTED|FIXED|COVER|NEW)\s+(\S+)(?:\s+([-+]?\d+))?\b",
    re.IGNORECASE,
)

SHAPE_RE = re.compile(r"\+\s*SHAPE\s+(\S+)", re.IGNORECASE)
COORD_RE = re.compile(r"\(\s*([-+]?\d+)\s+([-+]?\d+)\s*\)")
DO_BY_STEP_RE = re.compile(
    r"\bDO\s+(\d+)\s+BY\s+(\d+)\s+STEP\s+([-+]?\d+)\s+([-+]?\d+)\b",
    re.IGNORECASE)

# Words that can appear after coords but should not be mistaken as via names.
# This list is not exhaustive, but helps avoid obvious false positives.
NOT_A_VIA_TOKENS = {
    "RECT",
    "POLYGON",
    "MASK",
    "TAPER",
    "TAPERRULE",
    "STYLE",
    "PROPERTY",
    "+",
}

# -----------------------------
# Data models
# -----------------------------


@dataclass(frozen=True)
class Segment:
    net: str
    layer: str
    shape: str  # STRIPE / FOLLOWPIN / etc
    width: Optional[int]  # width from clause (may be None)
    start: Tuple[int, int]
    end: Tuple[int, int]
    direction: str  # vertical / horizontal / diagonal / point
    position: Optional[int]  # x for vertical, y for horizontal, else None
    length: float
    clause: str  # full clause text for reporting


@dataclass(frozen=True)
class Via:
    net: str
    layer: str
    via_type: str
    x: int
    y: int
    clause: str


# -----------------------------
# Parsing helpers
# -----------------------------


def _classify_segment(p1: Tuple[int, int],
                      p2: Tuple[int, int]) -> Tuple[str, Optional[int], float]:
    x1, y1 = p1
    x2, y2 = p2

    if x1 == x2 and y1 == y2:
        return "point", None, 0.0
    if x1 == x2:
        return "vertical", x1, float(abs(y2 - y1))
    if y1 == y2:
        return "horizontal", y1, float(abs(x2 - x1))
    return "diagonal", None, math.hypot(x2 - x1, y2 - y1)


def _infer_pitch(values: List[int]) -> Dict[str, Any]:
    """
    Infer a dominant pitch from a set of positions by:
      - unique sorting
      - consecutive diffs
      - mode(diff)

    Returns a dict with:
      pitch: Optional[int]
      unique_positions: List[int]
      diffs: List[int]
      diff_counts: Dict[int,int]
      mode_fraction: Optional[float]
    """
    vals = [int(v) for v in values if v is not None]
    uniq = sorted(set(vals))
    if len(uniq) < 2:
        return {
            "pitch": None,
            "unique_positions": uniq,
            "diffs": [],
            "diff_counts": {},
            "mode_fraction": None,
        }

    diffs = [uniq[i + 1] - uniq[i] for i in range(len(uniq) - 1)]
    diffs = [d for d in diffs if d != 0]
    if not diffs:
        return {
            "pitch": None,
            "unique_positions": uniq,
            "diffs": [],
            "diff_counts": {},
            "mode_fraction": None,
        }

    counts = Counter(diffs)
    pitch, pitch_count = counts.most_common(1)[0]
    mode_fraction = pitch_count / len(diffs) if diffs else None

    return {
        "pitch": pitch,
        "unique_positions": uniq,
        "diffs": diffs,
        "diff_counts": dict(counts),
        "mode_fraction": mode_fraction,
    }


def _most_common_or_none(values: List[Any]) -> Optional[Any]:
    if not values:
        return None
    return Counter(values).most_common(1)[0][0]


def _split_net_into_clauses(net_lines: List[str]) -> List[str]:
    """
    Take a single SPECIALNETS net block (lines from '- VDD ...' to terminating ';')
    and split into routing clauses starting with ROUTED/FIXED/COVER/NEW.

    This supports:
      - multi-line clauses
      - multiple clauses on one line (e.g. "... viaName NEW met2 ...")
    """
    # Exclude the net header line "- VDD (...) + USE POWER"
    body = " ".join(line.strip() for line in net_lines[1:])
    body = re.sub(r";\s*$", "", body).strip()
    if not body:
        return []

    starts = [m.start() for m in CLAUSE_START_RE.finditer(body)]
    if not starts:
        return []

    clauses: List[str] = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else len(body)
        clause = body[s:e].strip()
        clauses.append(clause)
    return clauses


def _parse_clause_to_segments_and_vias(
        net: str, clause: str) -> Tuple[List[Segment], List[Via]]:
    """
    Parse one SPECIALNETS routing clause into:
      - zero or more geometric segments (STRIPE/FOLLOWPIN polylines -> consecutive segments)
      - zero or more via instances (via name trailing after a coordinate)

    Fix #2 is implemented here: via is *not* "SHAPE VIA". It's the trailing
    token after coords: "( x y ) viaName".
    """
    m = CLAUSE_HEADER_RE.match(clause)
    if not m:
        return [], []

    _kind = m.group(1).upper()
    layer = m.group(2)
    width = int(m.group(3)) if m.group(3) is not None else None

    shape_m = SHAPE_RE.search(clause)
    shape = shape_m.group(1).upper() if shape_m else "UNKNOWN"

    coord_matches = list(COORD_RE.finditer(clause))
    points = [(int(cm.group(1)), int(cm.group(2))) for cm in coord_matches]

    segments: List[Segment] = []
    vias: List[Via] = []

    # Build segments for STRIPE/FOLLOWPIN shapes when we have >=2 points.
    # FOLLOWPIN is treated exactly like STRIPE (Fix #1 is in analysis, but parsing is shared).
    if shape in {"STRIPE", "FOLLOWPIN"} and len(points) >= 2:
        for i in range(len(points) - 1):
            p1 = points[i]
            p2 = points[i + 1]
            direction, position, length = _classify_segment(p1, p2)
            segments.append(
                Segment(
                    net=net,
                    layer=layer,
                    shape=shape,
                    width=width,
                    start=p1,
                    end=p2,
                    direction=direction,
                    position=position,
                    length=length,
                    clause=clause,
                ))

    # Detect vias from trailing tokens after the last coordinate.
    # Example:
    #   NEW met3 0 + SHAPE STRIPE ( 2039200 2017980 ) via3_1920x980
    # This is a via at (2039200, 2017980) of type "via3_1920x980"
    via_type: Optional[str] = None
    do_by_step: Optional[Tuple[int, int, int, int]] = None

    if points and coord_matches:
        tail = clause[coord_matches[-1].end():].strip()
        if tail:
            # If tail starts with "+", it's likely properties, not a via.
            if not tail.startswith("+"):
                first_token = tail.split()[0]
                if first_token and first_token.upper() not in NOT_A_VIA_TOKENS:
                    # Heuristic: treat as via name
                    via_type = first_token

                    # Expand via arrays if present:
                    #   viaName DO nx BY ny STEP dx dy
                    dm = DO_BY_STEP_RE.search(tail)
                    if dm:
                        nx, ny, dx, dy = map(int, dm.groups())
                        do_by_step = (nx, ny, dx, dy)

    if via_type:
        x0, y0 = points[-1]
        if do_by_step:
            nx, ny, dx, dy = do_by_step
            for ix in range(nx):
                for iy in range(ny):
                    vias.append(
                        Via(net=net,
                            layer=layer,
                            via_type=via_type,
                            x=x0 + ix * dx,
                            y=y0 + iy * dy,
                            clause=clause))
        else:
            vias.append(
                Via(net=net,
                    layer=layer,
                    via_type=via_type,
                    x=x0,
                    y=y0,
                    clause=clause))

    return segments, vias


def iter_specialnet_blocks(
        def_file_path: str) -> Iterable[Tuple[str, List[str]]]:
    """
    Yield (net_name, net_lines) for each net inside SPECIALNETS.
    net_lines includes the starting '- NET ...' line and the ending line containing ';'.
    """
    in_specialnets = False
    current_net: Optional[str] = None
    net_lines: List[str] = []

    with open(def_file_path, "r", errors="ignore") as f:
        for raw_line in f:
            line = raw_line.rstrip("\n")

            if not in_specialnets:
                if SPECIALNETS_START_RE.match(line):
                    in_specialnets = True
                continue

            if SPECIALNETS_END_RE.match(line):
                # Flush unterminated net (if any)
                if current_net and net_lines:
                    yield current_net, net_lines
                break

            m = NET_START_RE.match(line)
            if m:
                # Flush previous net if it wasn't properly terminated
                if current_net and net_lines:
                    yield current_net, net_lines
                current_net = m.group(1)
                net_lines = [line]
                # Some weird DEFs might end net header with ';' (rare)
                if NET_END_RE.search(line):
                    yield current_net, net_lines
                    current_net = None
                    net_lines = []
                continue

            if current_net is not None:
                net_lines.append(line)
                if NET_END_RE.search(line):
                    yield current_net, net_lines
                    current_net = None
                    net_lines = []


# -----------------------------
# Analysis
# -----------------------------


def _summarize_segments_for_net_layer(
    net_layer_key: str,
    segments: List[Segment],
) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    """
    Summarize stripe/followpin patterns and detect outliers.

    FOLLOWPIN is treated exactly like STRIPE here (Fix #1).
    """
    outliers: List[Dict[str, Any]] = []

    # group by shape then by direction
    by_shape: Dict[str, List[Segment]] = defaultdict(list)
    for s in segments:
        by_shape[s.shape].append(s)

    summary: Dict[str, Any] = {}

    for shape, segs in sorted(by_shape.items()):
        if shape not in {"STRIPE", "FOLLOWPIN"}:
            # You can keep other shapes if needed; currently ignore in summary
            continue

        shape_summary: Dict[str, Any] = {
            "count": len(segs),
            "directions": dict(Counter(s.direction for s in segs)),
        }

        by_dir: Dict[str, List[Segment]] = defaultdict(list)
        for s in segs:
            by_dir[s.direction].append(s)

        # Flag diagonal/point segments as outliers (usually unexpected for PDN stripes/followpins).
        for bad_dir in ("diagonal", "point"):
            for s in by_dir.get(bad_dir, []):
                outliers.append({
                    "type": "segment_direction_outlier",
                    "net_layer": net_layer_key,
                    "shape": shape,
                    "expected": "horizontal_or_vertical",
                    "actual": s.direction,
                    "start": s.start,
                    "end": s.end,
                    "width": s.width,
                    "clause": s.clause,
                })

        # Analyze vertical and horizontal separately (more accurate than "majority direction").
        for direction in ("vertical", "horizontal"):
            dir_segs = by_dir.get(direction, [])
            if not dir_segs:
                continue

            widths = [s.width for s in dir_segs if s.width is not None]
            width_counts = Counter(widths) if widths else Counter()
            dominant_width = width_counts.most_common(
                1)[0][0] if width_counts else None

            positions = [
                s.position for s in dir_segs if s.position is not None
            ]
            pitch_info = _infer_pitch([p for p in positions if p is not None])

            # Per-direction summary
            shape_summary.setdefault("by_direction", {})[direction] = {
                "count": len(dir_segs),
                "dominant_width": dominant_width,
                "width_counts": dict(width_counts),
                "pitch": pitch_info["pitch"],
                "pitch_mode_fraction": pitch_info["mode_fraction"],
                "unique_positions": len(pitch_info["unique_positions"]),
                "diff_counts": pitch_info["diff_counts"],
            }

            # Width outliers
            if dominant_width is not None and width_counts and len(
                    width_counts) > 1:
                for s in dir_segs:
                    if s.width != dominant_width:
                        outliers.append({
                            "type": "segment_width_outlier",
                            "net_layer": net_layer_key,
                            "shape": shape,
                            "direction": direction,
                            "expected_width": dominant_width,
                            "actual_width": s.width,
                            "start": s.start,
                            "end": s.end,
                            "clause": s.clause,
                        })

            # Pitch outliers: alignment + gap
            pitch = pitch_info["pitch"]
            uniq_positions = pitch_info["unique_positions"]

            # Only attempt pitch-based outliers if we have enough data to infer a pattern.
            if pitch is not None and pitch > 0 and len(uniq_positions) >= 3:
                # Alignment outliers: positions should share the same remainder mod pitch
                remainders = [pos % pitch for pos in uniq_positions]
                expected_rem = _most_common_or_none(remainders)
                if expected_rem is not None:
                    bad_positions = {
                        pos
                        for pos in uniq_positions
                        if (pos % pitch) != expected_rem
                    }
                    if bad_positions:
                        for s in dir_segs:
                            if s.position in bad_positions:
                                outliers.append({
                                    "type":
                                    "segment_pitch_alignment_outlier",
                                    "net_layer":
                                    net_layer_key,
                                    "shape":
                                    shape,
                                    "direction":
                                    direction,
                                    "expected_pitch":
                                    pitch,
                                    "expected_mod":
                                    expected_rem,
                                    "actual_mod": (s.position % pitch)
                                    if s.position is not None else None,
                                    "position":
                                    s.position,
                                    "start":
                                    s.start,
                                    "end":
                                    s.end,
                                    "clause":
                                    s.clause,
                                })

                # Gap outliers: consecutive diffs should be multiples of the dominant pitch
                for i in range(len(uniq_positions) - 1):
                    a = uniq_positions[i]
                    b = uniq_positions[i + 1]
                    d = b - a
                    if d % pitch != 0:
                        outliers.append({
                            "type": "segment_pitch_gap_outlier",
                            "net_layer": net_layer_key,
                            "shape": shape,
                            "direction": direction,
                            "expected_pitch": pitch,
                            "gap": d,
                            "from_position": a,
                            "to_position": b,
                        })

        summary[shape] = shape_summary

    return summary, outliers


def _summarize_vias_for_net_layer(
    net_layer_key: str,
    vias: List[Via],
) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    outliers: List[Dict[str, Any]] = []
    if not vias:
        return {}, outliers

    by_type: Dict[str, List[Via]] = defaultdict(list)
    for v in vias:
        by_type[v.via_type].append(v)

    via_summary: Dict[str, Any] = {"via_types": {}, "total_vias": len(vias)}

    # Via type outliers (rare types) - enabled only when one type clearly dominates
    type_counts = Counter(v.via_type for v in vias)
    dominant_type, dominant_count = type_counts.most_common(1)[0]
    total = len(vias)
    dominant_frac = dominant_count / total if total else 0.0

    flag_type_outliers = (total >= 20 and dominant_frac >= 0.8
                          and len(type_counts) > 1)

    for via_type, vlist in sorted(by_type.items()):
        xs = [v.x for v in vlist]
        ys = [v.y for v in vlist]

        pitch_x_info = _infer_pitch(xs)
        pitch_y_info = _infer_pitch(ys)

        pitch_x = pitch_x_info["pitch"]
        pitch_y = pitch_y_info["pitch"]

        via_summary["via_types"][via_type] = {
            "count": len(vlist),
            "pitch_x": pitch_x,
            "pitch_x_mode_fraction": pitch_x_info["mode_fraction"],
            "pitch_y": pitch_y,
            "pitch_y_mode_fraction": pitch_y_info["mode_fraction"],
            "unique_x": len(pitch_x_info["unique_positions"]),
            "unique_y": len(pitch_y_info["unique_positions"]),
            "x_diff_counts": pitch_x_info["diff_counts"],
            "y_diff_counts": pitch_y_info["diff_counts"],
        }

        if flag_type_outliers and via_type != dominant_type:
            for v in vlist:
                outliers.append({
                    "type": "via_type_outlier",
                    "net_layer": net_layer_key,
                    "expected_via_type": dominant_type,
                    "actual_via_type": via_type,
                    "x": v.x,
                    "y": v.y,
                    "clause": v.clause,
                })

        # Pitch alignment / gap outliers per axis, per via type
        # Only when we have enough points to infer a meaningful pitch.
        # X axis
        if pitch_x is not None and pitch_x > 0 and len(set(xs)) >= 3:
            uniq_x = sorted(set(xs))
            rems = [x % pitch_x for x in uniq_x]
            expected_rem = _most_common_or_none(rems)
            if expected_rem is not None:
                bad_x = {x for x in uniq_x if (x % pitch_x) != expected_rem}
                for v in vlist:
                    if v.x in bad_x:
                        outliers.append({
                            "type": "via_pitch_x_alignment_outlier",
                            "net_layer": net_layer_key,
                            "via_type": via_type,
                            "expected_pitch_x": pitch_x,
                            "expected_mod": expected_rem,
                            "actual_mod": v.x % pitch_x,
                            "x": v.x,
                            "y": v.y,
                            "clause": v.clause,
                        })
            for i in range(len(uniq_x) - 1):
                d = uniq_x[i + 1] - uniq_x[i]
                if d % pitch_x != 0:
                    outliers.append({
                        "type": "via_pitch_x_gap_outlier",
                        "net_layer": net_layer_key,
                        "via_type": via_type,
                        "expected_pitch_x": pitch_x,
                        "gap": d,
                        "from_x": uniq_x[i],
                        "to_x": uniq_x[i + 1],
                    })

        # Y axis
        if pitch_y is not None and pitch_y > 0 and len(set(ys)) >= 3:
            uniq_y = sorted(set(ys))
            rems = [y % pitch_y for y in uniq_y]
            expected_rem = _most_common_or_none(rems)
            if expected_rem is not None:
                bad_y = {y for y in uniq_y if (y % pitch_y) != expected_rem}
                for v in vlist:
                    if v.y in bad_y:
                        outliers.append({
                            "type": "via_pitch_y_alignment_outlier",
                            "net_layer": net_layer_key,
                            "via_type": via_type,
                            "expected_pitch_y": pitch_y,
                            "expected_mod": expected_rem,
                            "actual_mod": v.y % pitch_y,
                            "x": v.x,
                            "y": v.y,
                            "clause": v.clause,
                        })
            for i in range(len(uniq_y) - 1):
                d = uniq_y[i + 1] - uniq_y[i]
                if d % pitch_y != 0:
                    outliers.append({
                        "type": "via_pitch_y_gap_outlier",
                        "net_layer": net_layer_key,
                        "via_type": via_type,
                        "expected_pitch_y": pitch_y,
                        "gap": d,
                        "from_y": uniq_y[i],
                        "to_y": uniq_y[i + 1],
                    })

    return via_summary, outliers


def analyze_def_pdn(def_file_path: str,
                    *,
                    keep_raw: bool = False) -> Dict[str, Any]:
    """
    Main analysis entry point.
    """
    segments_by_net_layer: Dict[Tuple[str, str],
                                List[Segment]] = defaultdict(list)
    vias_by_net_layer: Dict[Tuple[str, str], List[Via]] = defaultdict(list)

    # Parse SPECIALNETS net blocks -> clauses -> segments/vias
    for net, net_lines in iter_specialnet_blocks(def_file_path):
        clauses = _split_net_into_clauses(net_lines)
        for clause in clauses:
            segs, vias = _parse_clause_to_segments_and_vias(net, clause)
            for s in segs:
                segments_by_net_layer[(s.net, s.layer)].append(s)
            for v in vias:
                vias_by_net_layer[(v.net, v.layer)].append(v)

    patterns: Dict[str, Any] = {}
    outliers: List[Dict[str, Any]] = []

    all_keys = sorted(
        set(segments_by_net_layer.keys()) | set(vias_by_net_layer.keys()))
    for (net, layer) in all_keys:
        key = f"{net}-{layer}"

        segs = segments_by_net_layer.get((net, layer), [])
        vs = vias_by_net_layer.get((net, layer), [])

        seg_summary, seg_outliers = _summarize_segments_for_net_layer(
            key, segs)
        via_summary, via_outliers = _summarize_vias_for_net_layer(key, vs)

        patterns[key] = {
            "summary": {
                "segments": seg_summary,
                "vias": via_summary,
            }
        }
        if keep_raw:
            patterns[key]["raw"] = {
                "segments": [s.__dict__ for s in segs],
                "vias": [v.__dict__ for v in vs],
            }

        outliers.extend(seg_outliers)
        outliers.extend(via_outliers)

    return {"patterns": patterns, "outliers": outliers}


# -----------------------------
# Printing
# -----------------------------


def _fmt_counts(counter_dict: Dict[Any, int], max_items: int = 6) -> str:
    if not counter_dict:
        return "{}"
    items = sorted(counter_dict.items(), key=lambda kv: (-kv[1], kv[0]))
    shown = items[:max_items]
    more = len(items) - len(shown)
    s = ", ".join(f"{k}:{v}" for k, v in shown)
    if more > 0:
        s += f", ... (+{more} more)"
    return "{" + s + "}"


def print_analysis_results(results: Dict[str, Any],
                           *,
                           max_outliers: int = 100) -> None:
    print("=" * 100)
    print("PDN SPECIALNETS Analysis")
    print("=" * 100)
    print()

    patterns = results.get("patterns", {})
    outliers = results.get("outliers", [])

    if not patterns:
        print("No patterns found (SPECIALNETS missing or no routings parsed).")
        return

    print("Pattern Summary (per net-layer)")
    print("-" * 100)

    for net_layer, data in patterns.items():
        print(f"\nNet-Layer: {net_layer}")

        seg_summary = data["summary"].get("segments", {})
        via_summary = data["summary"].get("vias", {})

        # Segments: STRIPE + FOLLOWPIN
        for shape in ("STRIPE", "FOLLOWPIN"):
            if shape not in seg_summary:
                continue
            ssum = seg_summary[shape]
            print(
                f"  {shape}: count={ssum.get('count', 0)} directions={_fmt_counts(ssum.get('directions', {}))}"
            )

            by_dir = ssum.get("by_direction", {})
            for direction in ("vertical", "horizontal"):
                if direction not in by_dir:
                    continue
                dsum = by_dir[direction]
                print(f"    {direction}: count={dsum['count']}")
                print(f"      dominant_width: {dsum['dominant_width']}")
                print(
                    f"      width_counts: {_fmt_counts(dsum['width_counts'])}")
                if dsum["pitch"] is not None:
                    print(
                        f"      inferred_pitch: {dsum['pitch']} (mode_fraction={dsum['pitch_mode_fraction']:.3f})"
                    )
                    print(
                        f"      diff_counts: {_fmt_counts(dsum['diff_counts'])}"
                    )
                else:
                    print(
                        "      inferred_pitch: N/A (not enough unique positions)"
                    )

        # Vias
        if via_summary:
            print(f"  VIAs: total={via_summary.get('total_vias', 0)}")
            via_types = via_summary.get("via_types", {})
            for vt, vinfo in via_types.items():
                print(f"    {vt}: count={vinfo['count']}")
                print(
                    f"      pitch_x={vinfo['pitch_x']} (mode_fraction={vinfo['pitch_x_mode_fraction']}) unique_x={vinfo['unique_x']}"
                )
                print(
                    f"      pitch_y={vinfo['pitch_y']} (mode_fraction={vinfo['pitch_y_mode_fraction']}) unique_y={vinfo['unique_y']}"
                )

    print("\n" + "-" * 100)
    if outliers:
        print(
            f"Outliers Detected: {len(outliers)} (showing up to {max_outliers})"
        )
        print("-" * 100)
        for o in outliers[:max_outliers]:
            print(f"\nType: {o.get('type')}")
            print(f"Net-Layer: {o.get('net_layer')}")
            # Print key fields if present
            for field in (
                    "shape",
                    "direction",
                    "via_type",
                    "expected_width",
                    "actual_width",
                    "expected_pitch",
                    "gap",
                    "position",
                    "x",
                    "y",
                    "expected_via_type",
                    "actual_via_type",
            ):
                if field in o:
                    print(f"{field}: {o[field]}")
            if "clause" in o:
                # avoid printing extremely long clauses unbounded
                clause = o["clause"]
                if len(clause) > 300:
                    clause = clause[:300] + " ...[truncated]"
                print(f"Clause: {clause}")
    else:
        print("No outliers detected.")

    print("\n" + "=" * 100)


# -----------------------------
# CLI
# -----------------------------


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Analyze PDN stripes/followpins/vias in DEF SPECIALNETS.")
    ap.add_argument("def_file", help="Path to DEF file")
    ap.add_argument("--max-outliers",
                    type=int,
                    default=120,
                    help="Max outliers to print")
    ap.add_argument(
        "--keep-raw",
        action="store_true",
        help="Store raw segments/vias in the result dict (memory heavy)")
    args = ap.parse_args()

    results = analyze_def_pdn(args.def_file, keep_raw=args.keep_raw)
    print_analysis_results(results, max_outliers=args.max_outliers)


if __name__ == "__main__":
    main()
