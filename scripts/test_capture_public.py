"""Offline synthetic boundary fixtures; these are not venue payload evidence."""
import gzip
import http.client
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("capture_public", Path(__file__).with_name("capture_public.py"))
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


class PublicCaptureBoundaries(unittest.TestCase):
    def test_truncated_content_length_never_returns_partial_body(self):
        class SocketFixture:
            def makefile(self, _mode):
                return io.BytesIO(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort")
        response = http.client.HTTPResponse(SocketFixture())
        response.begin()
        with self.assertRaisesRegex(capture.Refused, "http_incomplete_body"):
            capture.read_http_body(response)

    def test_gzip_expansion_is_refused_without_partial_result(self):
        payload = gzip.compress(b"x" * (capture.MAX_PAYLOAD + 1))
        with self.assertRaisesRegex(capture.Refused, "gzip_expanded_payload_limit"):
            capture.decode_payload(payload, "gzip")

    def test_incomplete_or_concatenated_gzip_is_refused(self):
        payload = gzip.compress(b"fixture")
        for invalid in (payload[:-1], payload + payload):
            with self.subTest(bytes=len(invalid)), self.assertRaises(capture.Refused):
                capture.decode_payload(invalid, "gzip")

    def test_artifact_budget_rejects_whole_record_and_preserves_index(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = capture.Bundle(Path(temporary) / "bundle", {}, b"{}")
            original_limit = capture.MAX_BYTES
            try:
                bundle.record(b"first", transport="fixture")
                capture.MAX_BYTES = capture.RESERVE + bundle.used
                with self.assertRaisesRegex(capture.Refused, "artifact_limit"):
                    bundle.record(b"second", transport="fixture")
            finally:
                capture.MAX_BYTES = original_limit
                bundle.finish("fixture", "synthetic")
            self.assertEqual((bundle.directory / "frames.bin").read_bytes(), b"first")
            rows = (bundle.directory / "frames.jsonl").read_text().splitlines()
            self.assertEqual(len(rows), 1)
            self.assertEqual(json.loads(rows[0])["length"], 5)
            self.assertLessEqual(sum(path.stat().st_size for path in bundle.directory.iterdir()), capture.MAX_BYTES)

    def test_unavailable_route_never_selects_fallback(self):
        profile = {"schema": "exchange.public_capture.routes",
                   "venues": {"fixture": {"products": {"spot": {
                       "status": "unavailable", "reason": "public_access_not_confirmed"}}}}}
        with self.assertRaisesRegex(capture.Refused, "public_access_not_confirmed"):
            capture.select_routes(profile, "fixture", "all")


if __name__ == "__main__":
    unittest.main()
