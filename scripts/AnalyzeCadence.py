#!/usr/bin/env python3
"""Offline cadence analysis for a completed exchange_api_probe capture."""

from array import array
import argparse
import csv
import html
import json
import math
import os
from pathlib import Path
import sqlite3
import sys
import tempfile


SCHEMA_VERSION = 1
MAX_PATH_BYTES = 512 * 1024 * 1024
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
UINT64_MAX = (1 << 64) - 1
CSV_FIELDS = (
    "receive_ns",
    "publish_ns",
    "event_ns",
    "transaction_ns",
    "id",
    "first_id",
    "last_id",
    "previous_id",
)
TIMESTAMP_FIELDS = CSV_FIELDS[:4]
PERCENTILES = (50, 90, 95, 99)
PAUSE_MS = (100, 250, 500, 1000)
SOURCE_CLEAN = {"ok", "success", "complete", "observed"}
SOURCE_EMPTY = {"empty", "no_data"}
ZERO_ID = "0" * 20


class AnalysisError(RuntimeError):
    pass


def percentile(values, percent):
    """Return a linearly interpolated percentile, including 0/100 boundaries."""
    if not 0 <= percent <= 100:
        raise ValueError("percent must be in [0, 100]")
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100.0
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def _rounded(value):
    if value is None:
        return None
    result = round(float(value), 6)
    return 0.0 if result == 0 else result


def _distribution_from_sorted(ordered):
    if not ordered:
        return None
    return {
        "count": len(ordered),
        "min": _rounded(ordered[0]),
        "p50": _rounded(_percentile_sorted(ordered, 50)),
        "p90": _rounded(_percentile_sorted(ordered, 90)),
        "p95": _rounded(_percentile_sorted(ordered, 95)),
        "p99": _rounded(_percentile_sorted(ordered, 99)),
        "max": _rounded(ordered[-1]),
    }


def _percentile_sorted(ordered, percent):
    position = (len(ordered) - 1) * percent / 100.0
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


class _TimestampTracker:
    def __init__(self):
        self.present = 0
        self.missing = 0
        self.negative = 0
        self.previous = None
        self.intervals_ms = array("d")
        self.pause_counts = {threshold: 0 for threshold in PAUSE_MS}

    def add(self, value):
        if value == 0:
            self.missing += 1
            self.previous = None
            return
        self.present += 1
        if self.previous is not None:
            delta = value - self.previous
            if delta < 0:
                self.negative += 1
            else:
                milliseconds = delta / 1_000_000.0
                self.intervals_ms.append(milliseconds)
                for threshold in PAUSE_MS:
                    if milliseconds > threshold:
                        self.pause_counts[threshold] += 1
        self.previous = value

    def finish(self):
        ordered = sorted(self.intervals_ms)
        summary = {
            "present": self.present,
            "missing": self.missing,
            "interval_count": len(ordered),
            "negative_deltas": self.negative,
            "interarrival_ms": _distribution_from_sorted(ordered),
            "pauses": {
                "over_100_ms": self.pause_counts[100],
                "over_250_ms": self.pause_counts[250],
                "over_500_ms": self.pause_counts[500],
                "over_1000_ms": self.pause_counts[1000],
            },
        }
        return summary, ordered


def summarize_timestamps(values):
    tracker = _TimestampTracker()
    for value in values:
        tracker.add(value)
    return tracker.finish()[0]


def _canonical_feed_name(name, path_kind):
    value = str(name)
    kind = str(path_kind)
    for separator in (".", "/", ":", "_", "-"):
        prefix = kind + separator
        suffix = separator + kind
        if value.startswith(prefix):
            value = value[len(prefix):]
            break
        if value.endswith(suffix):
            value = value[:-len(suffix)]
            break
    return value


def _uint64(value, context):
    if isinstance(value, bool):
        raise ValueError(f"{context} is not uint64")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{context} is not uint64") from error
    if str(value).strip() != str(parsed) or not 0 <= parsed <= UINT64_MAX:
        raise ValueError(f"{context} is not uint64")
    return parsed


def _metadata_count(lane, field, errors):
    try:
        return _uint64(lane.get(field, 0), field)
    except ValueError as error:
        errors.append(str(error))
        return 0


class _EventStore:
    def __init__(self, filename):
        self.connection = sqlite3.connect(filename)
        self.connection.execute("PRAGMA journal_mode=OFF")
        self.connection.execute("PRAGMA synchronous=OFF")
        self.connection.execute("PRAGMA temp_store=MEMORY")
        self.connection.execute("PRAGMA cache_size=-8192")
        self.connection.execute(
            """CREATE TABLE events (
                lane INTEGER NOT NULL,
                event_id TEXT NOT NULL,
                receive_ns TEXT NOT NULL,
                publish_ns TEXT NOT NULL,
                first_id TEXT NOT NULL,
                last_id TEXT NOT NULL,
                PRIMARY KEY (lane, event_id)
            ) WITHOUT ROWID"""
        )

    def add(self, lane_index, record):
        event_id = record["id"]
        if event_id == 0:
            return False
        cursor = self.connection.execute(
            "INSERT OR IGNORE INTO events VALUES (?, ?, ?, ?, ?, ?)",
            (
                lane_index,
                f"{event_id:020d}",
                str(record["receive_ns"]),
                str(record["publish_ns"]),
                f"{record['first_id']:020d}",
                f"{record['last_id']:020d}",
            ),
        )
        return cursor.rowcount == 1

    def commit(self):
        self.connection.commit()

    def close(self):
        self.connection.close()

    def contiguous_gaps(self, lane_index):
        previous = None
        gap_events = 0
        missing_ids = 0
        query = "SELECT event_id FROM events WHERE lane=? ORDER BY event_id"
        for (event_id,) in self.connection.execute(query, (lane_index,)):
            current = int(event_id)
            if previous is not None and current > previous + 1:
                gap_events += 1
                missing_ids += current - previous - 1
            previous = current
        return gap_events, missing_ids


class _LaneAccumulator:
    def __init__(self, feed, start_ns, duration_ns, lane_index, store):
        self.feed = feed
        self.start_ns = start_ns
        self.end_ns = start_ns + duration_ns
        self.lane_index = lane_index
        self.store = store
        self.records = 0
        self.timestamps = {field: _TimestampTracker() for field in TIMESTAMP_FIELDS}
        self.first_receive = None
        self.last_receive = None
        self.second_counts = {}
        self.out_of_window = 0
        self.nonzero_ids = 0
        self.unique_ids = 0
        self.duplicates = 0
        self.order_regressions = 0
        self.previous_id = None
        self.gap_events = 0
        self.missing_ids = 0
        self.previous_last_id = None
        self.pu_checks = 0
        self.pu_breaks = 0
        self.contiguous = feed in {"trade", "aggTrade"}
        self.depth = feed.startswith("diff_depth_") or feed.startswith("partial_depth_")

    def add(self, record):
        receive = record["receive_ns"]
        if receive and not self.start_ns <= receive < self.end_ns:
            self.out_of_window += 1
            return
        self.records += 1
        for field in TIMESTAMP_FIELDS:
            self.timestamps[field].add(record[field])
        if receive:
            if self.first_receive is None:
                self.first_receive = receive
            self.last_receive = receive
            bucket = (receive - self.start_ns) // 1_000_000_000
            self.second_counts[int(bucket)] = self.second_counts.get(int(bucket), 0) + 1

        event_id = record["id"]
        if event_id:
            self.nonzero_ids += 1
            if self.store.add(self.lane_index, record):
                self.unique_ids += 1
            else:
                self.duplicates += 1
            if self.previous_id is not None:
                if event_id < self.previous_id:
                    self.order_regressions += 1
            self.previous_id = event_id

        if self.depth and record["previous_id"] and self.previous_last_id is not None:
            self.pu_checks += 1
            if record["previous_id"] != self.previous_last_id:
                self.pu_breaks += 1
        if self.depth and record["last_id"]:
            self.previous_last_id = record["last_id"]

    def finish(self, duration_ns):
        timestamp_summaries = {}
        receive_ordered = []
        for field, tracker in self.timestamps.items():
            timestamp_summaries[field], ordered = tracker.finish()
            if field == "receive_ns":
                receive_ordered = ordered
        if self.first_receive is None:
            gaps = {
                "beginning": None,
                "end": None,
                "empty_window": _rounded(duration_ns / 1_000_000.0),
            }
        else:
            gaps = {
                "beginning": _rounded(max(0, self.first_receive - self.start_ns) / 1_000_000.0),
                "end": _rounded(max(0, self.end_ns - self.last_receive) / 1_000_000.0),
                "empty_window": None,
            }
        if self.contiguous:
            self.gap_events, self.missing_ids = self.store.contiguous_gaps(
                self.lane_index
            )
        ids = {
            "nonzero": self.nonzero_ids,
            "unique": self.unique_ids,
            "duplicates": self.duplicates,
            "order_regressions": self.order_regressions,
            "contiguous_expected": self.contiguous,
            "gap_events": self.gap_events if self.contiguous else None,
            "missing_ids": self.missing_ids if self.contiguous else None,
            "pu_checks": self.pu_checks,
            "pu_breaks": self.pu_breaks,
        }
        cdf = _cdf_points(receive_ordered)
        return timestamp_summaries, gaps, ids, {
            "second_counts": self.second_counts,
            "receive_cdf": cdf,
        }


def _cdf_points(ordered, maximum_points=201):
    if not ordered:
        return []
    count = len(ordered)
    if count <= maximum_points:
        indices = range(count)
    else:
        indices = sorted({round(i * (count - 1) / (maximum_points - 1))
                          for i in range(maximum_points)})
    return [[_rounded(ordered[index]), _rounded((index + 1) * 100.0 / count)]
            for index in indices]


def _resolve_csv(capture_dir, csv_name):
    if not csv_name:
        return None
    root = capture_dir.resolve()
    candidate = (capture_dir / str(csv_name)).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ValueError("CSV path escapes capture directory") from error
    return candidate


def _read_lane(lane, lane_index, capture_dir, start_ns, duration_ns, store):
    errors = []
    name = str(lane.get("name", f"lane-{lane_index}"))
    path_kind = str(lane.get("path_kind", "unknown"))
    feed = _canonical_feed_name(name, path_kind)
    accumulator = _LaneAccumulator(feed, start_ns, duration_ns, lane_index, store)
    csv_errors = 0
    csv_path = None
    try:
        csv_path = _resolve_csv(capture_dir, lane.get("csv", ""))
    except ValueError as error:
        errors.append(str(error))

    if csv_path is not None:
        if not csv_path.is_file():
            errors.append("CSV file is missing")
        else:
            try:
                with csv_path.open("r", newline="", encoding="utf-8") as source:
                    reader = csv.DictReader(source)
                    if tuple(reader.fieldnames or ()) != CSV_FIELDS:
                        errors.append("CSV header does not match cadence schema")
                    else:
                        for row_number, raw in enumerate(reader, start=2):
                            try:
                                record = {
                                    field: _uint64(raw[field], f"CSV row {row_number} {field}")
                                    for field in CSV_FIELDS
                                }
                            except (KeyError, ValueError):
                                csv_errors += 1
                                continue
                            accumulator.add(record)
            except (OSError, UnicodeError, csv.Error) as error:
                errors.append(f"CSV read failed: {error}")

    store.commit()
    timestamps, gaps, ids, chart = accumulator.finish(duration_ns)
    source_status = str(lane.get("status", "unknown"))
    frames_available = bool(lane.get("frames_available", path_kind == "direct"))
    frames = _metadata_count(lane, "frames", errors)
    parse_errors = _metadata_count(lane, "parse_errors", errors)
    disconnects = _metadata_count(lane, "disconnects", errors)
    control_pings = _metadata_count(lane, "control_pings", errors)
    overflow = _metadata_count(lane, "overflow", errors)
    warmup_events = _metadata_count(lane, "warmup_events", errors)
    warmup_parse_errors = _metadata_count(lane, "warmup_parse_errors", errors)

    reasons = list(errors)
    if csv_errors:
        reasons.append(f"{csv_errors} malformed CSV row(s)")
    if accumulator.out_of_window:
        reasons.append(f"{accumulator.out_of_window} out-of-window row(s)")
    if parse_errors:
        reasons.append(f"{parse_errors} capture parse error(s)")
    if warmup_parse_errors:
        reasons.append(f"{warmup_parse_errors} warm-up parse error(s)")
    if disconnects:
        reasons.append(f"{disconnects} terminal route/disconnect event(s)")
    if overflow:
        reasons.append(f"{overflow} capture overflow event(s)")
    if ids["duplicates"]:
        reasons.append(f"{ids['duplicates']} duplicate nonzero ID(s)")
    if ids["order_regressions"]:
        reasons.append(f"{ids['order_regressions']} ID order regression(s)")
    if ids["gap_events"]:
        reasons.append(
            f"{ids['gap_events']} contiguous ID gap event(s), "
            f"{ids['missing_ids']} missing ID(s)"
        )
    if ids["pu_breaks"]:
        reasons.append(f"{ids['pu_breaks']} depth pu continuity break(s)")
    for field in ("receive_ns", "publish_ns"):
        local = timestamps[field]
        if local["missing"]:
            reasons.append(f"{local['missing']} missing {field} value(s)")
        if local["negative_deltas"]:
            reasons.append(
                f"{local['negative_deltas']} negative {field} delta(s)"
            )

    integrity_failure = bool(
        warmup_parse_errors
        or ids["duplicates"]
        or ids["order_regressions"]
        or ids["gap_events"]
        or ids["pu_breaks"]
        or timestamps["receive_ns"]["missing"]
        or timestamps["receive_ns"]["negative_deltas"]
        or timestamps["publish_ns"]["missing"]
        or timestamps["publish_ns"]["negative_deltas"]
    )

    if source_status == "unsupported":
        analysis_status = "unsupported"
    elif overflow or source_status == "incomplete":
        analysis_status = "incomplete"
    elif (errors or csv_errors or accumulator.out_of_window or parse_errors
          or disconnects or integrity_failure):
        analysis_status = "degraded"
    elif source_status in SOURCE_EMPTY or (source_status in SOURCE_CLEAN and accumulator.records == 0):
        analysis_status = "empty"
    elif source_status in SOURCE_CLEAN:
        analysis_status = "ok"
    else:
        analysis_status = source_status

    seconds = duration_ns / 1_000_000_000.0
    result = {
        "name": name,
        "feed": feed,
        "path_kind": path_kind,
        "source_status": source_status,
        "analysis_status": analysis_status,
        "clean": analysis_status == "ok",
        "error": str(lane.get("error", "")),
        "analysis_reasons": reasons,
        "parser": str(lane.get("parser", "")),
        "endpoint": str(lane.get("endpoint", "")),
        "subscription": str(lane.get("subscription", "")),
        "csv": str(lane.get("csv", "")),
        "frames_available": frames_available,
        "frames_semantics": str(lane.get("frames_semantics", "")),
        "frames": frames,
        "record_count": accumulator.records,
        "parse_errors": parse_errors,
        "csv_errors": csv_errors,
        "out_of_window_records": accumulator.out_of_window,
        "disconnects": disconnects,
        "control_pings": control_pings,
        "overflow": overflow,
        "warmup_events": warmup_events,
        "warmup_parse_errors": warmup_parse_errors,
        "rates_per_second": {
            "messages": _rounded(frames / seconds) if frames_available else None,
            "events": _rounded(accumulator.records / seconds),
        },
        "window_gaps_ms": gaps,
        "timestamps": timestamps,
        "ids": ids,
    }
    return result, chart


def _coverage(numerator, denominator):
    if denominator == 0:
        return None
    return _rounded(numerator * 100.0 / denominator)


def _signed_distribution(values):
    return _distribution_from_sorted(sorted(values))


def _compare_direct_cxet(lanes, store):
    groups = {}
    for index, lane in enumerate(lanes):
        groups.setdefault(lane["feed"], {})[lane["path_kind"]] = (index, lane)
    comparisons = []
    for feed in sorted(groups):
        group = groups[feed]
        if "direct" not in group or "cxet" not in group:
            continue
        direct_index, direct = group["direct"]
        cxet_index, cxet = group["cxet"]
        receive_differences = array("d")
        publish_differences = array("d")
        matched = 0
        query = """SELECT d.receive_ns, c.receive_ns, d.publish_ns, c.publish_ns
                   FROM events AS d JOIN events AS c ON d.event_id=c.event_id
                   WHERE d.lane=? AND c.lane=?"""
        for direct_receive, cxet_receive, direct_publish, cxet_publish in store.connection.execute(
                query, (direct_index, cxet_index)):
            matched += 1
            direct_receive, cxet_receive = int(direct_receive), int(cxet_receive)
            direct_publish, cxet_publish = int(direct_publish), int(cxet_publish)
            if direct_receive and cxet_receive:
                receive_differences.append((cxet_receive - direct_receive) / 1_000_000.0)
            if direct_publish and cxet_publish:
                publish_differences.append((cxet_publish - direct_publish) / 1_000_000.0)
        direct_unique = direct["ids"]["unique"]
        cxet_unique = cxet["ids"]["unique"]
        comparisons.append({
            "feed": feed,
            "direct_lane": direct["name"],
            "cxet_lane": cxet["name"],
            "direct_unique_ids": direct_unique,
            "cxet_unique_ids": cxet_unique,
            "matched_unique_ids": matched,
            "direct_coverage_percent": _coverage(matched, direct_unique),
            "cxet_coverage_percent": _coverage(matched, cxet_unique),
            "difference_definition": "cxet timestamp minus direct timestamp for a matched unique nonzero ID",
            "signed_differences_ms": {
                "receive_cxet_minus_direct": _signed_distribution(receive_differences),
                "publish_cxet_minus_direct": _signed_distribution(publish_differences),
            },
        })
    return comparisons


def _compare_trade_agg(lanes, store):
    groups = {}
    for index, lane in enumerate(lanes):
        groups.setdefault(lane["path_kind"], {})[lane["feed"]] = (index, lane)
    comparisons = []
    for path_kind in sorted(groups):
        group = groups[path_kind]
        if "trade" not in group or "aggTrade" not in group:
            continue
        trade_index, trade = group["trade"]
        agg_index, agg = group["aggTrade"]
        aggregate_ranges = 0
        first_matches = 0
        last_matches = 0
        first_differences = array("d")
        last_differences = array("d")
        query = """SELECT a.receive_ns,
                          first.event_id, first.receive_ns,
                          last.event_id, last.receive_ns
                   FROM events AS a
                   LEFT JOIN events AS first
                     ON first.lane=? AND first.event_id=a.first_id
                   LEFT JOIN events AS last
                     ON last.lane=? AND last.event_id=a.last_id
                   WHERE a.lane=? AND a.first_id!=? AND a.last_id!=?"""
        for agg_receive, first_id, first_receive, last_id, last_receive in store.connection.execute(
                query, (trade_index, trade_index, agg_index, ZERO_ID, ZERO_ID)):
            aggregate_ranges += 1
            agg_receive = int(agg_receive)
            if first_id is not None:
                first_matches += 1
                first_receive = int(first_receive)
                if agg_receive and first_receive:
                    first_differences.append((agg_receive - first_receive) / 1_000_000.0)
            if last_id is not None:
                last_matches += 1
                last_receive = int(last_receive)
                if agg_receive and last_receive:
                    last_differences.append((agg_receive - last_receive) / 1_000_000.0)
        comparisons.append({
            "path_kind": path_kind,
            "trade_lane": trade["name"],
            "agg_trade_lane": agg["name"],
            "raw_trade_unique_ids": trade["ids"]["unique"],
            "aggregate_ranges": aggregate_ranges,
            "first_id_matches": first_matches,
            "last_id_matches": last_matches,
            "first_id_coverage_percent": _coverage(first_matches, aggregate_ranges),
            "last_id_coverage_percent": _coverage(last_matches, aggregate_ranges),
            "difference_definition": "aggTrade receive_ns minus raw trade receive_ns; negative means aggTrade was received first",
            "signed_receipt_differences_ms": {
                "agg_minus_first_trade": _signed_distribution(first_differences),
                "agg_minus_last_trade": _signed_distribution(last_differences),
            },
        })
    return comparisons


def _validate_manifest(capture_dir):
    manifest_path = capture_dir / "manifest.json"
    if not manifest_path.is_file():
        raise AnalysisError(f"manifest not found: {manifest_path}")
    if manifest_path.stat().st_size > MAX_MANIFEST_BYTES:
        raise AnalysisError("manifest exceeds 4 MiB")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AnalysisError(f"cannot read manifest: {error}") from error
    if not isinstance(manifest, dict):
        raise AnalysisError("manifest must be a JSON object")
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise AnalysisError(f"unsupported schema_version: {manifest.get('schema_version')!r}")
    lanes = manifest.get("lanes")
    if not isinstance(lanes, list):
        raise AnalysisError("manifest lanes must be an array")
    if len(lanes) > 38:
        raise AnalysisError("more than 38 lanes")
    try:
        start_ns = _uint64(manifest.get("start_monotonic_ns"), "start_monotonic_ns")
        duration_seconds = float(manifest.get("duration_seconds"))
        warmup_seconds = float(manifest.get("warmup_seconds"))
    except (TypeError, ValueError) as error:
        raise AnalysisError(str(error)) from error
    if not 0 < duration_seconds <= 86_400 or not 0 <= warmup_seconds <= 86_400:
        raise AnalysisError("duration_seconds or warmup_seconds is out of range")
    duration_ns = int(duration_seconds * 1_000_000_000)
    if start_ns + duration_ns > UINT64_MAX:
        raise AnalysisError("capture window overflows uint64")

    path_sizes = {}
    path_files = {}
    lane_pairs = set()
    csv_files = set()
    for lane in lanes:
        if not isinstance(lane, dict):
            raise AnalysisError("each lane must be a JSON object")
        path_kind = lane.get("path_kind")
        if path_kind not in {"direct", "cxet"}:
            raise AnalysisError(f"invalid path_kind: {path_kind!r}")
        name = lane.get("name")
        if not isinstance(name, str) or not name:
            raise AnalysisError("lane name must be a nonempty string")
        pair = (_canonical_feed_name(name, path_kind), path_kind)
        if pair in lane_pairs:
            raise AnalysisError(f"duplicate feed/path: {pair[0]} {pair[1]}")
        lane_pairs.add(pair)
        csv_name = lane.get("csv", "")
        if csv_name:
            csv_key = str((capture_dir / str(csv_name)).resolve())
            if csv_key in csv_files:
                raise AnalysisError(f"duplicate CSV: {csv_name}")
            csv_files.add(csv_key)
        try:
            path = _resolve_csv(capture_dir, csv_name)
        except ValueError:
            continue
        if path is None or not path.is_file():
            continue
        kind = path_kind
        key = str(path)
        files = path_files.setdefault(kind, set())
        if key in files:
            continue
        files.add(key)
        path_sizes[kind] = path_sizes.get(kind, 0) + path.stat().st_size
    oversized = {kind: size for kind, size in path_sizes.items() if size > MAX_PATH_BYTES}
    if oversized:
        description = ", ".join(f"{kind}={size}" for kind, size in sorted(oversized.items()))
        raise AnalysisError(f"CSV input exceeds 512 MiB path limit: {description}")
    return manifest, start_ns, duration_ns, duration_seconds, warmup_seconds


def _atomic_write(path, text):
    temporary_name = None
    try:
        with tempfile.NamedTemporaryFile(
                "w", encoding="utf-8", dir=path.parent,
                prefix=f".{path.name}.", delete=False) as output:
            temporary_name = output.name
            output.write(text)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    except Exception:
        if temporary_name:
            try:
                os.unlink(temporary_name)
            except OSError:
                pass
        raise


def _format_number(value):
    return "—" if value is None else str(value)


def _markdown_cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def _report(summary):
    lines = [
        f"# Cadence report: {_markdown_cell(summary['symbol'])}",
        "",
        f"Measured window: {summary['duration_seconds']} s; warm-up: {summary['warmup_seconds']} s; "
        f"receive clock: `{summary['receive_clock']}`.",
        "",
        "Every manifest lane is retained below. `messages/s` is unavailable when `frames_available` is false. "
        "Empty distributions are shown as —, never as zero milliseconds.",
        "",
        "| lane | path | subscription | status | events | messages/s | events/s | receive p50 ms | receive p99 ms | receive max ms | negative receive deltas | >100/250/500/1000 ms | begin/end gap ms | duplicate IDs | order regressions | ID gaps | pu breaks/checks |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for lane in summary["lanes"]:
        receive = lane["timestamps"]["receive_ns"]
        dist = receive["interarrival_ms"] or {}
        pauses = receive["pauses"]
        gaps = lane["window_gaps_ms"]
        ids = lane["ids"]
        gap_text = "—" if ids["gap_events"] is None else f"{ids['gap_events']}/{ids['missing_ids']}"
        lines.append("| " + " | ".join(map(_markdown_cell, (
            lane["name"], lane["path_kind"], lane["subscription"],
            lane["analysis_status"], lane["record_count"],
            _format_number(lane["rates_per_second"]["messages"]),
            lane["rates_per_second"]["events"], _format_number(dist.get("p50")),
            _format_number(dist.get("p99")), _format_number(dist.get("max")),
            receive["negative_deltas"],
            f"{pauses['over_100_ms']}/{pauses['over_250_ms']}/{pauses['over_500_ms']}/{pauses['over_1000_ms']}",
            f"{_format_number(gaps['beginning'])}/{_format_number(gaps['end'])}",
            ids["duplicates"], ids["order_regressions"], gap_text,
            f"{ids['pu_breaks']}/{ids['pu_checks']}",
        ))) + " |")

    lines.extend(["", "## Direct and CXET matched IDs", ""])
    if summary["lane_comparisons"]:
        lines.extend([
            "| feed | direct unique | CXET unique | matched | direct coverage % | CXET coverage % | receive CXET-direct p50 ms | publish CXET-direct p50 ms |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ])
        for comparison in summary["lane_comparisons"]:
            receive = comparison["signed_differences_ms"]["receive_cxet_minus_direct"] or {}
            publish = comparison["signed_differences_ms"]["publish_cxet_minus_direct"] or {}
            lines.append("| " + " | ".join(map(_markdown_cell, (
                comparison["feed"], comparison["direct_unique_ids"], comparison["cxet_unique_ids"],
                comparison["matched_unique_ids"], _format_number(comparison["direct_coverage_percent"]),
                _format_number(comparison["cxet_coverage_percent"]), _format_number(receive.get("p50")),
                _format_number(publish.get("p50")),
            ))) + " |")
    else:
        lines.append("No direct/CXET feed pair was present in the manifest.")

    lines.extend(["", "## Raw trade and aggregate trade ranges", ""])
    if summary["trade_agg_comparisons"]:
        lines.extend([
            "| path | aggregate ranges | first matches/coverage % | last matches/coverage % | agg-first p50 ms | agg-last p50 ms |",
            "|---|---:|---:|---:|---:|---:|",
        ])
        for comparison in summary["trade_agg_comparisons"]:
            first = comparison["signed_receipt_differences_ms"]["agg_minus_first_trade"] or {}
            last = comparison["signed_receipt_differences_ms"]["agg_minus_last_trade"] or {}
            lines.append("| " + " | ".join(map(_markdown_cell, (
                comparison["path_kind"], comparison["aggregate_ranges"],
                f"{comparison['first_id_matches']}/{_format_number(comparison['first_id_coverage_percent'])}",
                f"{comparison['last_id_matches']}/{_format_number(comparison['last_id_coverage_percent'])}",
                _format_number(first.get("p50")), _format_number(last.get("p50")),
            ))) + " |")
    else:
        lines.append("No path contained both raw trade and aggTrade lanes.")

    lines.extend([
        "",
        "## Interpretation boundaries",
        "",
        "- Interarrival statistics describe activity cadence; they do not measure CPU parse time or prove a speedup.",
        "- Direct/CXET matching uses unique nonzero IDs. Signed differences are CXET timestamp minus direct timestamp.",
        "- Raw trades are paired to aggTrade `f` and `l` range endpoints. The aggTrade event ID is not treated as a raw trade ID.",
        "- ID gaps count absent integers inside the observed `trade`/`aggTrade` ID range. Sparse `bookTicker` IDs are not gaps. Gaps alone do not attribute a loss cause.",
        "- Binance CXET trade normalization intentionally skips zero-price/zero-quantity markers; the direct counter includes their raw IDs. These captures omit p/q, so unmatched IDs cannot distinguish marker filtering from other omissions.",
        "- Depth continuity checks `pu` against the preceding observed `last_id` only when `pu` is present.",
        "- Exchange `E`/`T` timestamps and local monotonic receive/publish timestamps remain separate clock domains.",
        "- The `disconnects` counter records capture terminal route/disconnect events. On CXET it can reflect local route recovery or terminal publication failure; it does not alone prove a TCP disconnect.",
        "",
        "See `summary.json` for all receive/publish/E/T distributions and `plots.svg` for per-second counts and receive-interval CDFs.",
        "",
    ])
    return "\n".join(lines)


def _svg(summary, charts):
    width = 1400
    height = 1050
    left, right = 75, 35
    plot_width = width - left - right
    top_one, panel_height = 65, 320
    top_two = 465
    colors = ("#2563eb", "#dc2626", "#059669", "#7c3aed", "#ea580c", "#0891b2",
              "#4f46e5", "#be123c", "#65a30d", "#9333ea", "#0f766e", "#b45309")
    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<style>text{font-family:system-ui,sans-serif;fill:#172033}.title{font-size:22px;font-weight:700}.axis{font-size:12px}.legend{font-size:11px}.grid{stroke:#dbe3ef;stroke-width:1}.axisline{stroke:#596579;stroke-width:1}</style>',
        '<text x="28" y="32" class="title">Cadence: measured events per second</text>',
    ]
    duration = max(1, int(math.ceil(summary["duration_seconds"])))
    maximum_count = max((max(chart["second_counts"].values(), default=0) for chart in charts), default=0)
    maximum_count = max(1, maximum_count)

    def axes(top, x_label, y_label):
        bottom = top + panel_height
        elements.extend([
            f'<line x1="{left}" y1="{bottom}" x2="{left + plot_width}" y2="{bottom}" class="axisline"/>',
            f'<line x1="{left}" y1="{top}" x2="{left}" y2="{bottom}" class="axisline"/>',
            f'<text x="{left + plot_width / 2}" y="{bottom + 38}" text-anchor="middle" class="axis">{html.escape(x_label)}</text>',
            f'<text x="18" y="{top + panel_height / 2}" text-anchor="middle" transform="rotate(-90 18 {top + panel_height / 2})" class="axis">{html.escape(y_label)}</text>',
        ])
        for step in range(5):
            y = bottom - step * panel_height / 4
            elements.append(f'<line x1="{left}" y1="{y}" x2="{left + plot_width}" y2="{y}" class="grid"/>')

    axes(top_one, "seconds from measured-window start", "events / second")
    for step in range(5):
        y = top_one + panel_height - step * panel_height / 4
        value = maximum_count * step / 4
        elements.append(f'<text x="{left - 8}" y="{y + 4}" text-anchor="end" class="axis">{value:.1f}</text>')
    active_count = 0
    for index, (lane, chart) in enumerate(zip(summary["lanes"], charts)):
        if not chart["second_counts"]:
            continue
        active_count += 1
        points = []
        for second in range(duration):
            x = left + (second + 0.5) * plot_width / duration
            y = top_one + panel_height - chart["second_counts"].get(second, 0) * panel_height / maximum_count
            points.append(f"{x:.2f},{y:.2f}")
        color = colors[index % len(colors)]
        elements.append(f'<polyline points="{" ".join(points)}" fill="none" stroke="{color}" stroke-width="1.6" opacity="0.82"/>')
    if active_count == 0:
        elements.append(f'<text x="{left + 20}" y="{top_one + 35}" class="axis">No measured receive timestamps</text>')

    elements.append('<text x="28" y="432" class="title">Receive interarrival CDF</text>')
    axes(top_two, "receive interarrival milliseconds (log1p scale)", "CDF percent")
    maximum_interval = max((points[-1][0] for points in (chart["receive_cdf"] for chart in charts) if points), default=0)
    maximum_interval = max(1.0, maximum_interval)
    cdf_active = 0
    for index, chart in enumerate(charts):
        if not chart["receive_cdf"]:
            continue
        cdf_active += 1
        points = []
        for milliseconds, percentage in chart["receive_cdf"]:
            x = left + math.log1p(milliseconds) / math.log1p(maximum_interval) * plot_width
            y = top_two + panel_height - percentage / 100.0 * panel_height
            points.append(f"{x:.2f},{y:.2f}")
        color = colors[index % len(colors)]
        elements.append(f'<polyline points="{" ".join(points)}" fill="none" stroke="{color}" stroke-width="1.6" opacity="0.82"/>')
    if cdf_active == 0:
        elements.append(f'<text x="{left + 20}" y="{top_two + 35}" class="axis">No nonnegative receive intervals</text>')
    for step in range(5):
        y = top_two + panel_height - step * panel_height / 4
        elements.append(f'<text x="{left - 8}" y="{y + 4}" text-anchor="end" class="axis">{step * 25}</text>')

    legend_top = 865
    columns = 3
    rows = max(1, math.ceil(len(summary["lanes"]) / columns))
    row_height = min(18, 150 / rows)
    column_width = (width - 60) / columns
    for index, lane in enumerate(summary["lanes"]):
        column = index // rows
        row = index % rows
        x = 30 + column * column_width
        y = legend_top + row * row_height
        color = colors[index % len(colors)] if charts[index]["second_counts"] else "#9aa4b2"
        label = f"{lane['path_kind']}:{lane['feed']} [{lane['analysis_status']}]"
        elements.append(f'<line x1="{x}" y1="{y}" x2="{x + 18}" y2="{y}" stroke="{color}" stroke-width="2"/>')
        elements.append(f'<text x="{x + 24}" y="{y + 4}" class="legend">{html.escape(label)}</text>')
    elements.append('</svg>')
    return "\n".join(elements) + "\n"


def analyze_capture(capture_dir):
    capture_dir = Path(capture_dir)
    manifest, start_ns, duration_ns, duration_seconds, warmup_seconds = _validate_manifest(capture_dir)
    lanes = []
    charts = []
    with tempfile.TemporaryDirectory(
            prefix=".cadence-analysis-", dir=capture_dir) as temporary:
        store = _EventStore(str(Path(temporary) / "events.sqlite"))
        try:
            for index, lane in enumerate(manifest["lanes"]):
                result, chart = _read_lane(
                    lane, index, capture_dir, start_ns, duration_ns, store
                )
                lanes.append(result)
                charts.append(chart)
            lane_comparisons = _compare_direct_cxet(lanes, store)
            trade_agg_comparisons = _compare_trade_agg(lanes, store)
        finally:
            store.close()

    summary = {
        "schema_version": SCHEMA_VERSION,
        "symbol": str(manifest.get("symbol", "")),
        "start_monotonic_ns": start_ns,
        "duration_seconds": duration_seconds,
        "warmup_seconds": warmup_seconds,
        "receive_clock": "CLOCK_MONOTONIC_RAW",
        "input_limit_bytes_per_path": MAX_PATH_BYTES,
        "counter_semantics": {
            "disconnects": (
                "capture terminal route/disconnect events; a CXET event can be "
                "local route recovery or terminal publication failure and does "
                "not by itself prove a TCP disconnect"
            )
        },
        "lanes": lanes,
        "lane_comparisons": lane_comparisons,
        "trade_agg_comparisons": trade_agg_comparisons,
    }
    _atomic_write(
        capture_dir / "summary.json",
        json.dumps(summary, indent=2, sort_keys=False) + "\n",
    )
    _atomic_write(capture_dir / "REPORT.md", _report(summary))
    _atomic_write(capture_dir / "plots.svg", _svg(summary, charts))
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Analyze an exchange_api_probe cadence capture offline"
    )
    parser.add_argument("capture_dir", type=Path)
    arguments = parser.parse_args(argv)
    try:
        summary = analyze_capture(arguments.capture_dir)
    except AnalysisError as error:
        parser.exit(2, f"AnalyzeCadence.py: {error}\n")
    print(
        f"wrote summary.json, REPORT.md and plots.svg for "
        f"{len(summary['lanes'])} lanes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
