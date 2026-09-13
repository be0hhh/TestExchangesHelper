#!/usr/bin/env python3

import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "AnalyzeCadence.py"
sys.dont_write_bytecode = True


def temporary_directory():
    return tempfile.TemporaryDirectory(dir=ROOT)


def load_analyzer():
    spec = importlib.util.spec_from_file_location("cadence_analysis", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


FIELDS = (
    "receive_ns",
    "publish_ns",
    "event_ns",
    "transaction_ns",
    "id",
    "first_id",
    "last_id",
    "previous_id",
)


def write_csv(directory, name, rows):
    path = Path(directory) / name
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    return name


def row(receive, *, publish=0, event=0, transaction=0, event_id=0,
        first=0, last=0, previous=0):
    return {
        "receive_ns": receive,
        "publish_ns": publish,
        "event_ns": event,
        "transaction_ns": transaction,
        "id": event_id,
        "first_id": first,
        "last_id": last,
        "previous_id": previous,
    }


def lane(name, path_kind, csv_name="", **overrides):
    value = {
        "name": name,
        "path_kind": path_kind,
        "status": "ok",
        "error": "",
        "parser": "fixture",
        "endpoint": "fixture",
        "subscription": "fixture",
        "frames": 0,
        "frames_available": path_kind == "direct",
        "frames_semantics": "decoded market-data WebSocket messages; fragmentation/control excluded",
        "parse_errors": 0,
        "warmup_parse_errors": 0,
        "disconnects": 0,
        "control_pings": 0,
        "overflow": 0,
        "warmup_events": 0,
        "csv": csv_name,
    }
    value.update(overrides)
    return value


def manifest(lanes, *, start=1_000_000_000, duration=2):
    return {
        "schema_version": 1,
        "symbol": "ETHUSDT",
        "start_monotonic_ns": start,
        "duration_seconds": duration,
        "warmup_seconds": 1,
        "lanes": lanes,
    }


class CadenceAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.analysis = load_analyzer()

    def test_percentile_interpolates_and_keeps_boundaries(self):
        percentile = self.analysis.percentile
        self.assertIsNone(percentile([], 50))
        self.assertEqual(0, percentile([0, 10, 20, 30], 0))
        self.assertEqual(15, percentile([0, 10, 20, 30], 50))
        self.assertEqual(28.5, percentile([0, 10, 20, 30], 95))
        self.assertEqual(30, percentile([0, 10, 20, 30], 100))

    def test_manifest_rejects_unknown_schema_version(self):
        with temporary_directory() as temporary:
            data = manifest([])
            data["schema_version"] = 2
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            with self.assertRaisesRegex(self.analysis.AnalysisError, "schema_version"):
                self.analysis.analyze_capture(Path(temporary))

    def test_timestamp_summary_excludes_missing_and_separates_regressions(self):
        summary = self.analysis.summarize_timestamps(
            [1_000_000_000, 0, 1_100_000_000, 1_050_000_000, 2_050_000_000]
        )
        self.assertEqual(4, summary["present"])
        self.assertEqual(1, summary["missing"])
        self.assertEqual(1, summary["negative_deltas"])
        self.assertEqual(1, summary["interval_count"])
        self.assertEqual(1000.0, summary["interarrival_ms"]["p50"])
        self.assertEqual(1, summary["pauses"]["over_100_ms"])
        self.assertEqual(1, summary["pauses"]["over_500_ms"])
        self.assertEqual(0, summary["pauses"]["over_1000_ms"])

    def test_empty_lane_has_full_empty_window_and_no_zero_interval_claim(self):
        with temporary_directory() as temporary:
            csv_name = write_csv(temporary, "empty.csv", [])
            data = manifest([lane("trade", "direct", csv_name)])
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            summary = self.analysis.analyze_capture(Path(temporary))
            result = summary["lanes"][0]

            self.assertEqual("empty", result["analysis_status"])
            self.assertFalse(result["clean"])
            self.assertIsNone(
                result["timestamps"]["receive_ns"]["interarrival_ms"]
            )
            self.assertIsNone(result["window_gaps_ms"]["beginning"])
            self.assertIsNone(result["window_gaps_ms"]["end"])
            self.assertEqual(2000.0, result["window_gaps_ms"]["empty_window"])

    def test_duplicate_regression_gap_and_depth_pu_policies_are_distinct(self):
        start = 1_000_000_000
        with temporary_directory() as temporary:
            trade_csv = write_csv(
                temporary,
                "trade.csv",
                [
                    row(start + 1, event_id=10),
                    row(start + 2, event_id=10),
                    row(start + 3, event_id=13),
                    row(start + 4, event_id=11),
                ],
            )
            bbo_csv = write_csv(
                temporary,
                "bbo.csv",
                [row(start + 1, event_id=10), row(start + 2, event_id=15)],
            )
            depth_csv = write_csv(
                temporary,
                "depth.csv",
                [
                    row(start + 1, event_id=100, first=90, last=100),
                    row(start + 2, event_id=110, first=101, last=110, previous=99),
                    row(start + 3, event_id=120, first=111, last=120, previous=110),
                ],
            )
            data = manifest(
                [
                    lane("trade", "direct", trade_csv),
                    lane("bookTicker", "direct", bbo_csv),
                    lane("diff_depth_100ms", "direct", depth_csv),
                ],
                start=start,
            )
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            results = {
                item["name"]: item
                for item in self.analysis.analyze_capture(Path(temporary))["lanes"]
            }

            trade_ids = results["trade"]["ids"]
            self.assertEqual(1, trade_ids["duplicates"])
            self.assertEqual(1, trade_ids["order_regressions"])
            self.assertTrue(trade_ids["contiguous_expected"])
            self.assertEqual(1, trade_ids["gap_events"])
            self.assertEqual(1, trade_ids["missing_ids"])

            bbo_ids = results["bookTicker"]["ids"]
            self.assertFalse(bbo_ids["contiguous_expected"])
            self.assertIsNone(bbo_ids["gap_events"])
            self.assertIsNone(bbo_ids["missing_ids"])

            depth_ids = results["diff_depth_100ms"]["ids"]
            self.assertEqual(2, depth_ids["pu_checks"])
            self.assertEqual(1, depth_ids["pu_breaks"])

    def test_contiguous_gap_count_uses_sorted_unique_ids(self):
        start = 1_000_000_000
        with temporary_directory() as temporary:
            trade_csv = write_csv(
                temporary,
                "trade.csv",
                [
                    row(start + 1, event_id=10),
                    row(start + 2, event_id=12),
                    row(start + 3, event_id=11),
                    row(start + 4, event_id=13),
                ],
            )
            data = manifest([lane("trade", "direct", trade_csv)], start=start)
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            ids = self.analysis.analyze_capture(Path(temporary))["lanes"][0]["ids"]

            self.assertEqual(1, ids["order_regressions"])
            self.assertEqual(0, ids["gap_events"])
            self.assertEqual(0, ids["missing_ids"])

    def test_contiguous_id_gap_prevents_clean_source_status(self):
        start = 1_000_000_000
        with temporary_directory() as temporary:
            trade_csv = write_csv(
                temporary,
                "trade.csv",
                [
                    row(start + 1, publish=start + 2, event_id=10),
                    row(start + 3, publish=start + 4, event_id=12),
                ],
            )
            data = manifest(
                [lane("trade", "cxet", trade_csv, status="complete")],
                start=start,
            )
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            result = self.analysis.analyze_capture(Path(temporary))["lanes"][0]

            self.assertEqual("complete", result["source_status"])
            self.assertEqual("degraded", result["analysis_status"])
            self.assertFalse(result["clean"])
            self.assertIn("1 contiguous ID gap event(s), 1 missing ID(s)",
                          result["analysis_reasons"])

    def test_manifest_rejects_lane_bound_pair_and_csv_aliases(self):
        cases = {
            "more than 38 lanes": manifest(
                [lane(f"feed-{index}", "direct") for index in range(39)]
            ),
            "invalid path_kind": manifest([lane("trade", "other")]),
            "duplicate feed/path": manifest(
                [lane("trade", "direct"), lane("trade", "direct")]
            ),
            "duplicate CSV": manifest(
                [
                    lane("trade", "direct", "same.csv"),
                    lane("aggTrade", "direct", "same.csv"),
                ]
            ),
        }
        for expected, data in cases.items():
            with self.subTest(expected=expected), temporary_directory() as temporary:
                (Path(temporary) / "manifest.json").write_text(
                    json.dumps(data), encoding="utf-8"
                )
                with self.assertRaisesRegex(self.analysis.AnalysisError, expected):
                    self.analysis.analyze_capture(Path(temporary))

    def test_direct_cxet_matching_uses_unique_ids_and_signed_differences(self):
        start = 1_000_000_000
        ms = 1_000_000
        with temporary_directory() as temporary:
            direct_csv = write_csv(
                temporary,
                "direct.csv",
                [
                    row(start + 10 * ms, publish=start + 11 * ms, event_id=1),
                    row(start + 20 * ms, publish=start + 21 * ms, event_id=2),
                    row(start + 21 * ms, publish=start + 22 * ms, event_id=2),
                ],
            )
            cxet_csv = write_csv(
                temporary,
                "cxet.csv",
                [
                    row(start + 25 * ms, publish=start + 24 * ms, event_id=2),
                    row(start + 30 * ms, publish=start + 31 * ms, event_id=3),
                ],
            )
            data = manifest(
                [
                    lane("trade", "direct", direct_csv),
                    lane("trade", "cxet", cxet_csv),
                ],
                start=start,
            )
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            comparison = self.analysis.analyze_capture(Path(temporary))[
                "lane_comparisons"
            ][0]

            self.assertEqual("trade", comparison["feed"])
            self.assertEqual(2, comparison["direct_unique_ids"])
            self.assertEqual(2, comparison["cxet_unique_ids"])
            self.assertEqual(1, comparison["matched_unique_ids"])
            self.assertEqual(50.0, comparison["direct_coverage_percent"])
            self.assertEqual(50.0, comparison["cxet_coverage_percent"])
            self.assertEqual(
                5.0,
                comparison["signed_differences_ms"]["receive_cxet_minus_direct"][
                    "p50"
                ],
            )
            self.assertEqual(
                3.0,
                comparison["signed_differences_ms"]["publish_cxet_minus_direct"][
                    "p50"
                ],
            )

    def test_trade_agg_pairing_uses_first_and_last_ranges(self):
        start = 1_000_000_000
        ms = 1_000_000
        with temporary_directory() as temporary:
            trade_csv = write_csv(
                temporary,
                "trade.csv",
                [
                    row(start + 10 * ms, event_id=100),
                    row(start + 12 * ms, event_id=101),
                    row(start + 20 * ms, event_id=102),
                ],
            )
            agg_csv = write_csv(
                temporary,
                "agg.csv",
                [row(start + 15 * ms, event_id=77, first=100, last=102)],
            )
            data = manifest(
                [
                    lane("direct.trade", "direct", trade_csv),
                    lane("direct.aggTrade", "direct", agg_csv),
                ],
                start=start,
            )
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            result = self.analysis.analyze_capture(Path(temporary))[
                "trade_agg_comparisons"
            ][0]

            self.assertEqual(1, result["aggregate_ranges"])
            self.assertEqual(1, result["first_id_matches"])
            self.assertEqual(1, result["last_id_matches"])
            self.assertEqual(100.0, result["first_id_coverage_percent"])
            self.assertEqual(100.0, result["last_id_coverage_percent"])
            self.assertEqual(
                5.0,
                result["signed_receipt_differences_ms"][
                    "agg_minus_first_trade"
                ]["p50"],
            )
            self.assertEqual(
                -5.0,
                result["signed_receipt_differences_ms"][
                    "agg_minus_last_trade"
                ]["p50"],
            )

    def test_cli_keeps_nonclean_rows_and_writes_all_three_artifacts(self):
        with temporary_directory() as temporary:
            data = manifest(
                [
                    lane("trade", "direct", status="unsupported"),
                    lane("trade", "cxet", status="unavailable", error="no parser"),
                    lane("bookTicker", "direct", disconnects=1, overflow=1),
                ]
            )
            (Path(temporary) / "manifest.json").write_text(
                json.dumps(data), encoding="utf-8"
            )

            completed = subprocess.run(
                [sys.executable, str(SCRIPT), temporary],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(0, completed.returncode, completed.stderr)
            summary_path = Path(temporary) / "summary.json"
            report_path = Path(temporary) / "REPORT.md"
            plot_path = Path(temporary) / "plots.svg"
            self.assertTrue(summary_path.is_file())
            self.assertTrue(report_path.is_file())
            self.assertTrue(plot_path.is_file())
            output = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(3, len(output["lanes"]))
            self.assertTrue(all(not item["clean"] for item in output["lanes"]))
            self.assertEqual(
                "decoded market-data WebSocket messages; fragmentation/control excluded",
                output["lanes"][0]["frames_semantics"],
            )
            self.assertEqual(0, output["lanes"][0]["warmup_parse_errors"])
            self.assertIn("fixture", report_path.read_text(encoding="utf-8"))
            self.assertTrue(plot_path.read_text(encoding="utf-8").startswith("<svg"))


if __name__ == "__main__":
    unittest.main()
