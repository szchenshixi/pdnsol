#!/usr/bin/env python3
"""
Header Manager - A tool to generate, maintain, and update source file headers.
Keeps a text-based history database for tracking modified files.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple


@dataclass
class HeaderFields:
    file_name: str = ""
    version: str = ""
    author: str = ""
    created: str = ""
    last_modified: str = ""
    description: str = ""
    copyright_line: str = ""
    copyright_holder: str = ""
    copyright_year: str = ""


# @dataclass
# class HistoryEntry:
#     path: str
#     tags: Set[str] = field(default_factory=set)


class HistoryManager:
    """Maintain a history of processed file paths and action logs."""

    LOG_PATTERN = "header_manager_%Y%m%d-%H%M.log"

    def __init__(self, history_path: Path):
        self.history_path = Path(history_path).expanduser()
        self.previous_paths: Set[str] = set()
        self.session_actions: Dict[str, Set[str]] = {
            "new": set(),
            "updated": set(),
            "skipped": set(),
        }
        self._load()

    def _normalize(self, file_path: str) -> str:
        return str(Path(file_path).resolve())

    def _load(self) -> None:
        if not self.history_path.exists():
            return

        try:
            with open(self.history_path, "r", encoding="utf-8") as reader:
                for raw_line in reader:
                    line = raw_line.strip()
                    if line:
                        self.previous_paths.add(self._normalize(line))
        except OSError as exc:
            raise SystemExit(
                f"Failed to read history database '{self.history_path}': {exc}"
            ) from exc

    def record_action(self, action: str, file_path: str) -> None:
        normalized = self._normalize(file_path)
        if action in self.session_actions:
            self.session_actions[action].add(normalized)

    def finalize(self, current_paths: Iterable[str]) -> None:
        normalized_current = {self._normalize(path) for path in current_paths}

        missing_paths = sorted(self.previous_paths - normalized_current)
        updated_paths = sorted(self.session_actions["updated"])
        new_paths = sorted(self.session_actions["new"])
        skipped_paths = sorted(self.session_actions["skipped"])

        self._write(normalized_current)
        self._write_log(new_paths, updated_paths, skipped_paths, missing_paths)

        self.previous_paths = normalized_current
        for action_set in self.session_actions.values():
            action_set.clear()

    def _write(self, current_paths: Set[str]) -> None:
        try:
            self.history_path.parent.mkdir(parents=True, exist_ok=True)
            with open(self.history_path, "w", encoding="utf-8") as writer:
                if current_paths:
                    writer.write("\n".join(sorted(current_paths)) + "\n")
                else:
                    writer.write("")
        except OSError as exc:
            raise SystemExit(
                f"Failed to write history database '{self.history_path}': {exc}"
            ) from exc

    def _write_log(self, new: List[str], updated: List[str],
                   skipped: List[str], missing: List[str]) -> None:
        log_name = datetime.now().strftime(self.LOG_PATTERN)
        log_path = self.history_path.parent / log_name

        sections = [
            ("new", new),
            ("updated", updated),
            ("skipped", skipped),
            ("missing", missing),
        ]

        lines: List[str] = []

        for label, items in sections:
            lines.append(f"{label}:")
            for item in items:
                lines.append(f"    {item}")
            lines.append("")

        while lines and lines[-1] == "":
            lines.pop()

        log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(log_path, "w", encoding="utf-8") as writer:
            writer.write("\n".join(lines) + ("\n" if lines else ""))


class HeaderManager:
    COMMENT_STYLES: Dict[str, str] = {
        ".c": "//",
        ".cc": "//",
        ".cpp": "//",
        ".cxx": "//",
        ".h": "//",
        ".hh": "//",
        ".hpp": "//",
        ".hxx": "//",
        ".java": "//",
        ".js": "//",
        ".ts": "//",
        ".cs": "//",
        ".go": "//",
        ".swift": "//",
        ".kt": "//",
        ".m": "//",
        ".mm": "//",
        ".rs": "//",
        ".py": "#",
        ".pyw": "#",
        ".sh": "#",
        ".bash": "#",
        ".zsh": "#",
        ".ps1": "#",
        ".rb": "#",
        ".pl": "#",
        ".pm": "#",
        ".lua": "--",
        ".sql": "--",
    }

    DEFAULT_TEMPLATE: Dict[str, str] = {
        "author": "Your Name",
        "version": "1.0",
        "copyright_holder": "Your Company",
    }

    FIELD_LABEL_WIDTH = 15
    RULER_WIDTH = 78
    FIELD_ORDER: List[Tuple[str, str]] = [
        ("File Name", "file_name"),
        ("Version", "version"),
        ("Author", "author"),
        ("Created", "created"),
        ("Last Modified", "last_modified"),
        ("Description", "description"),
    ]
    FIELD_REGEX = re.compile(r"^(?P<label>[A-Za-z ]+?)\s*:\s*(?P<value>.*)$")
    COPYRIGHT_REGEX = re.compile(
        r"^Copyright\s*\(C\)\s*(?P<year>[\d\-]+),\s*(?P<holder>.+?)\s*-\s*All Rights Reserved$",
        re.IGNORECASE,
    )

    def __init__(self,
                 config: Optional[Dict[str, str]] = None,
                 history: Optional[HistoryManager] = None):
        self.config = config or {}
        self.history = history
        self._progress_enabled = sys.stdout.isatty()
        self._progress_bar_length = 30
        self._progress_bar_width = 0

    # --------------------------------------------------------------------- #
    # Public API                                                            #
    # --------------------------------------------------------------------- #
    def _record_history_action(self, action: str, file_path: str) -> None:
        if self.history:
            self.history.record_action(action, file_path)

    def process_directory(
        self,
        directory: str,
        description: Optional[str],
        extensions: Optional[Iterable[str]],
    ) -> List[str]:
        target_extensions = self._normalize_extensions(extensions)
        target_files = self._collect_target_files(directory, target_extensions)
        total_files = len(target_files)
        processed_count = 0
        new_files: List[str] = []
        updated_files: List[str] = []
        skipped_files: List[str] = []

        self._progress_bar_width = 0

        if total_files:
            self._print_progress_bar(0, total_files)

        for file_path in target_files:
            self._clear_progress_bar()

            action = self.update_header(file_path, description)

            if action == "new":
                new_files.append(file_path)
                print(f"New header: {file_path}")
            elif action == "updated":
                updated_files.append(file_path)
                print(f"Updated: {file_path}")
            else:
                skipped_files.append(file_path)
                print(f"Skipped: {file_path}")

            processed_count += 1
            self._print_progress_bar(processed_count, total_files)

        if self._progress_enabled and total_files:
            self._clear_progress_bar()

        progress_line = self._render_progress_bar(processed_count, total_files)
        print(progress_line)
        print(
            f"\nSummary: {len(updated_files)} updated, {len(new_files)} new, {len(skipped_files)} skipped."
        )
        return [str(Path(path).resolve()) for path in target_files]

    def update_header(self, file_path: str, description: Optional[str]) -> str:
        try:
            with open(file_path, "r", encoding="utf-8") as reader:
                original_content = reader.read()
        except UnicodeDecodeError:
            print(f"Skipping non-text/binary file: {file_path}")
            self._record_history_action("skipped", file_path)
            return "skipped"

        bom_present = original_content.startswith("\ufeff")
        content = original_content.lstrip("\ufeff")

        newline = self._detect_newline(content)
        comment_prefix = self._comment_prefix(file_path)

        if not comment_prefix:
            self._record_history_action("skipped", file_path)
            return "skipped"

        shebang, body = self._extract_shebang(content, comment_prefix)

        header_info, header_text, remainder = self._parse_header(
            body, comment_prefix)
        had_header = header_info is not None
        updated_header = self._generate_header(
            file_path=file_path,
            comment_prefix=comment_prefix,
            newline=newline,
            existing=header_info,
            description_override=description,
        )

        if had_header:
            new_body = self._join_header_and_body(
                updated_header,
                remainder,
                newline,
            )
        else:
            trimmed_body = body.lstrip()
            new_body = self._join_header_and_body(
                updated_header,
                trimmed_body,
                newline,
            )

        rebuilt = f"{shebang}{new_body}" if shebang else new_body
        if bom_present:
            rebuilt = "\ufeff" + rebuilt

        if rebuilt == original_content:
            self._record_history_action("skipped", file_path)
            return "skipped"

        with open(file_path, "w", encoding="utf-8", newline="") as writer:
            writer.write(rebuilt)

        action = "new" if not had_header else "updated"
        self._record_history_action(action, file_path)

        return action

    # --------------------------------------------------------------------- #
    # Header generation helpers                                             #
    # --------------------------------------------------------------------- #

    def _generate_header(
        self,
        file_path: str,
        comment_prefix: str,
        newline: str,
        existing: Optional[HeaderFields],
        description_override: Optional[str],
    ) -> str:
        now = datetime.now()
        now_str = now.strftime("%Y-%m-%d %H:%M")
        file_name = os.path.basename(file_path)

        config = {**self.DEFAULT_TEMPLATE, **self.config}

        header_fields = HeaderFields()
        header_fields.file_name = file_name
        header_fields.version = (
            config.get("version") if config.get("version") else
            (existing.version if existing and existing.version else
             self.DEFAULT_TEMPLATE["version"]))
        header_fields.author = (
            config.get("author") if config.get("author") else
            (existing.author if existing and existing.author else
             self.DEFAULT_TEMPLATE["author"]))
        header_fields.created = existing.created if existing and existing.created else now_str
        header_fields.last_modified = now_str
        header_fields.description = (
            existing.description if existing and existing.description else
            (description_override if description_override is not None else
             config.get("description", "")))

        if existing and existing.description and description_override:
            # Respect requirement: keep existing description.
            pass

        holder = (config.get("copyright_holder")
                  if config.get("copyright_holder") else
                  (existing.copyright_holder
                   if existing and existing.copyright_holder else
                   self.DEFAULT_TEMPLATE["copyright_holder"]))
        year = (config.get("copyright_year")
                if config.get("copyright_year") else
                (existing.copyright_year
                 if existing and existing.copyright_year else str(now.year)))

        copyright_line = (
            existing.copyright_line if existing and existing.copyright_line
            and not config.get("copyright_holder")
            and not config.get("copyright_year") else
            f"Copyright (C) {year}, {holder} - All Rights Reserved")

        ruler = comment_prefix + ("*" * self.RULER_WIDTH)

        lines: List[str] = [
            ruler,
            self._body_line(comment_prefix, copyright_line),
            ruler,
        ]

        for label, attr in self.FIELD_ORDER:
            value = getattr(header_fields, attr, "")
            lines.append(self._field_line(comment_prefix, label, value))

        lines.append(ruler)

        header = newline.join(lines) + newline
        return header

    def _body_line(self, prefix: str, text: str = "") -> str:
        return f"{prefix}  {text}" if text else prefix

    def _field_line(self, prefix: str, label: str, value: str) -> str:
        padded_label = f"{label:<{self.FIELD_LABEL_WIDTH}}"
        return self._body_line(prefix, f"{padded_label} : {value}".strip())

    def _join_header_and_body(self, header: str, body: str,
                              newline: str) -> str:
        if not body:
            return header
        if body.startswith(newline):
            return header + body
        return header + newline + body

    # --------------------------------------------------------------------- #
    # Header parsing                                                        #
    # --------------------------------------------------------------------- #

    def _parse_header(
        self,
        body: str,
        prefix: str,
    ) -> Tuple[Optional[HeaderFields], str, str]:
        working = body
        if working.startswith("\ufeff"):
            working = working.lstrip("\ufeff")

        lines = working.splitlines(keepends=True)
        header_lines: List[str] = []
        idx = 0

        for line in lines:
            if line.startswith(prefix):
                header_lines.append(line)
                idx += 1
            else:
                break

        if not header_lines:
            return None, "", working

        stripped_lines = [
            self._strip_prefix(line.rstrip("\r\n"), prefix)
            for line in header_lines
        ]

        if not stripped_lines:
            return None, "", working

        if not self._is_ruler(stripped_lines[0]) or not self._is_ruler(
                stripped_lines[-1]):
            return None, "", working

        extracted = self._extract_fields(stripped_lines)

        if not extracted:
            return None, "", working

        header_text = "".join(header_lines)
        remainder = "".join(lines[idx:])
        return extracted, header_text, remainder

    def _extract_fields(self,
                        stripped_lines: List[str]) -> Optional[HeaderFields]:
        fields = HeaderFields()
        found_any = False

        for line in stripped_lines:
            text = line.strip()
            if not text:
                continue
            if self._is_ruler(text):
                continue
            copyright_match = self.COPYRIGHT_REGEX.match(text)
            if copyright_match:
                fields.copyright_line = text
                fields.copyright_year = copyright_match.group("year").strip()
                fields.copyright_holder = copyright_match.group(
                    "holder").strip()
                found_any = True
                continue

            field_match = self.FIELD_REGEX.match(text)
            if not field_match:
                continue

            label = field_match.group("label").strip()
            value = field_match.group("value")

            key_map = {
                "File Name": "file_name",
                "Version": "version",
                "Author": "author",
                "Created": "created",
                "Last Modified": "last_modified",
                "Description": "description",
            }

            key = key_map.get(label)
            if key:
                setattr(fields, key, value)
                found_any = True

        essential = fields.file_name and fields.created and fields.last_modified
        if not essential:
            return None
        return fields if found_any else None

    # --------------------------------------------------------------------- #
    # Helper progress bar                                                   #
    # --------------------------------------------------------------------- #
    def _collect_target_files(
        self,
        directory: str,
        target_extensions: Iterable[str],
    ) -> List[str]:
        collected: List[str] = []
        for root, dirs, files in os.walk(directory):
            dirs[:] = [d for d in dirs if not d.startswith(".")]
            for file_name in files:
                file_path = os.path.join(root, file_name)
                if Path(file_path).suffix.lower() in target_extensions:
                    collected.append(file_path)
        collected.sort()
        return collected

    def _render_progress_bar(self, processed: int, total: int) -> str:
        if total <= 0:
            bar = "-" * self._progress_bar_length
            return f"Progress: [{bar}] 0/0 files"

        filled_length = min(
            self._progress_bar_length,
            int(self._progress_bar_length * processed / total),
        )
        bar = "#" * filled_length + "-" * (self._progress_bar_length -
                                           filled_length)
        return f"Progress: [{bar}] {processed}/{total} files"

    def _print_progress_bar(self, processed: int, total: int) -> None:
        if not self._progress_enabled or total <= 0:
            return

        bar_text = self._render_progress_bar(processed, total)
        self._progress_bar_width = max(self._progress_bar_width, len(bar_text))

        sys.stdout.write("\r" + bar_text.ljust(self._progress_bar_width))
        sys.stdout.flush()

    def _clear_progress_bar(self) -> None:
        if not self._progress_enabled or self._progress_bar_width == 0:
            return

        sys.stdout.write("\r" + " " * self._progress_bar_width + "\r")
        sys.stdout.flush()

    # --------------------------------------------------------------------- #
    # Utilities                                                             #
    # --------------------------------------------------------------------- #

    def _comment_prefix(self, file_path: str) -> Optional[str]:
        return self.COMMENT_STYLES.get(Path(file_path).suffix.lower())

    def _normalize_extensions(
            self, extensions: Optional[Iterable[str]]) -> List[str]:
        if extensions:
            normalized = []
            for ext in extensions:
                ext = ext.lower()
                if not ext.startswith("."):
                    ext = "." + ext
                if ext in self.COMMENT_STYLES:
                    normalized.append(ext)
            if normalized:
                return sorted(set(normalized))
        return sorted(self.COMMENT_STYLES.keys())

    def _strip_prefix(self, line: str, prefix: str) -> str:
        if line.startswith(prefix):
            stripped = line[len(prefix):]
            return stripped.lstrip()
        return line

    def _is_ruler(self, text: str) -> bool:
        cleaned = text.replace(" ", "")
        return bool(cleaned) and all(
            char == "*" for char in cleaned) and len(cleaned) >= 10

    def _detect_newline(self, content: str) -> str:
        if "\r\n" in content:
            return "\r\n"
        if "\r" in content:
            return "\r"
        return "\n"

    def _extract_shebang(self, content: str, prefix: str) -> Tuple[str, str]:
        if prefix != "#":
            return "", content
        if not content.startswith("#!"):
            return "", content

        newline_pos = content.find("\n")
        if newline_pos == -1:
            return content, ""
        return content[:newline_pos + 1], content[newline_pos + 1:]


# ------------------------------------------------------------------------- #
# CLI                                                                       #
# ------------------------------------------------------------------------- #


def load_config(path: str) -> Dict[str, str]:
    try:
        with open(path, "r", encoding="utf-8") as reader:
            return json.load(reader)
    except FileNotFoundError:
        return {}
    except json.JSONDecodeError as exc:
        raise SystemExit(
            f"Invalid JSON in config file '{path}': {exc}") from exc


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Manage source file headers.")
    parser.add_argument("path", help="Path to a file or directory.")
    parser.add_argument("--description",
                        "-d",
                        help="Description for new headers.")
    parser.add_argument("--author", "-a", help="Author name override.")
    parser.add_argument("--version", "-v", help="Version override.")
    parser.add_argument("--copyright", "-c", help="Copyright holder.")
    parser.add_argument("--copyright-year", help="Explicit copyright year.")
    parser.add_argument(
        "--extensions",
        "-e",
        nargs="+",
        help="File extensions to process (default: supported set).",
    )
    parser.add_argument(
        "--config",
        "-C",
        default="header_config.json",
        help="Path to a JSON configuration file (default: header_config.json).",
    )
    parser.add_argument(
        "--history",
        "-H",
        default=".header_history",
        help=
        "Path to the header history database file (default: .header_history).",
    )
    return parser


def main() -> None:
    parser = build_argument_parser()
    args = parser.parse_args()

    config = load_config(args.config)

    if args.author:
        config["author"] = args.author
    if args.version:
        config["version"] = args.version
    if args.description:
        config["description"] = args.description
    if args.copyright:
        config["copyright_holder"] = args.copyright
    if args.copyright_year:
        config["copyright_year"] = args.copyright_year

    history_path = Path(args.history).expanduser().resolve()
    history_manager = HistoryManager(history_path=history_path)

    manager = HeaderManager(config=config, history=history_manager)
    processed_paths: Set[str] = set()

    try:
        target_path = Path(args.path)

        if target_path.is_file():
            suffix = target_path.suffix.lower()
            if suffix not in HeaderManager.COMMENT_STYLES:
                print(f"Unsupported file type: {target_path}")
                history_manager.record_action("skipped",
                                              str(target_path.resolve()))
            else:
                resolved = str(target_path.resolve())
                processed_paths.add(resolved)
                action = manager.update_header(str(target_path),
                                               args.description)

                if action == "updated":
                    print(f"Successfully updated: {target_path}")
                elif action == "new":
                    print(f"Header added: {target_path}")
                else:
                    print(f"No changes made: {target_path}")
            return

        if target_path.is_dir():
            collected = manager.process_directory(str(target_path),
                                                  args.description,
                                                  args.extensions)
            processed_paths.update(collected)
            return

        raise SystemExit(f'Error: Path "{target_path}" does not exist.')
    finally:
        history_manager.finalize(processed_paths)


if __name__ == "__main__":
    main()
