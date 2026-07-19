from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


TOOL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_ROOT))

import catalog_audit  # noqa: E402
from model import WsCase  # noqa: E402
from redaction import public_frame, wire_shape  # noqa: E402
from rest_contracts import validate as validate_rest_contract  # noqa: E402
import sandbox  # noqa: E402
from venues import PRODUCTS, _kucoin_ack, _kucoin_data, _kucoin_welcome  # noqa: E402
from ws_client import _application_pong, _matches_data  # noqa: E402


class ProbeContractTests(unittest.TestCase):
    def test_reference_contracts_reject_http_200_logical_errors(self) -> None:
        ok, evidence = validate_rest_contract(
            "bitget_ticker24h", {"code": "40017", "msg": "invalid symbol"}, "BTCUSDT")
        self.assertFalse(ok)
        self.assertEqual("logical_error_or_empty_rows", evidence["error"])

    def test_reference_contracts_record_native_units(self) -> None:
        ok, evidence = validate_rest_contract(
            "bybit_ticker24h",
            {"retCode": 0, "result": {"list": [{"symbol": "BTCUSDT",
              "turnover24h": "10", "price24hPcnt": "0.0125"}]}}, "BTCUSDT")
        self.assertTrue(ok)
        self.assertEqual("turnover24h:quote", evidence["volume"])
        self.assertEqual("price24hPcnt:ratio", evidence["change_or_funding"])

    def test_binance_style_rest_is_uppercase_while_ws_stream_is_lowercase(self) -> None:
        for venue in ("binance", "aster"):
            product = next(item for item in PRODUCTS
                           if item.venue == venue and item.product == "futures")
            lowercase = next(case for case in product.public_rest
                             if case.name == "funding_lowercase")
            self.assertFalse(lowercase.expected_logical_success)
            funding_ws = next(case for case in product.public_ws
                              if case.name == "funding")
            self.assertIn(b"btcusdt", funding_ws.subscribe or funding_ws.path.encode())

    def test_kucoin_duplicate_funding_intervals_must_agree(self) -> None:
        base = {"symbol": "XBTUSDTM", "fundingFeeRate": "0.0001",
                "nextFundingRateDateTime": 1,
                "currentFundingRateGranularity": 28800000,
                "fundingRateGranularity": 28800000}
        self.assertTrue(validate_rest_contract(
            "kucoin_funding", {"code": "200000", "data": [base]}, "XBTUSDTM")[0])
        self.assertFalse(validate_rest_contract(
            "kucoin_funding", {"code": "200000", "data": [base | {
                "fundingRateGranularity": 14400000}]}, "XBTUSDTM")[0])

    def test_kucoin_profiles_require_binary_lifecycle(self) -> None:
        cases = [case for product in PRODUCTS if product.venue == "kucoin"
                 for case in product.public_ws]
        self.assertEqual(4, len(cases))
        for case in cases:
            self.assertTrue(case.read_before_subscribe)
            self.assertEqual(0x2, case.subscribe_opcode)
            self.assertEqual(0x2, case.expected_inbound_opcode)
            self.assertTrue(case.require_data_after_ack)
            self.assertEqual("core_aligned", case.status)

    def test_kucoin_validators_require_the_canonical_ack(self) -> None:
        self.assertTrue(_kucoin_welcome({"message": "welcome", "pingInterval": 1}, 0x2))
        self.assertFalse(_kucoin_welcome({"message": "welcome", "pingInterval": 1}, 0x1))
        self.assertTrue(_kucoin_ack({"id": "cxet-kucoin-1", "result": True}, 0x2))
        self.assertFalse(_kucoin_ack({"id": "other", "result": True}, 0x2))
        self.assertTrue(_kucoin_data({"topic": "trade"}, 0x2))
        self.assertFalse(_kucoin_data({"id": "cxet-kucoin-1", "result": True}, 0x2))

    def test_public_frame_preserves_text_or_encodes_binary(self) -> None:
        self.assertEqual({"encoding": "utf8", "value": '{"x":1}', "truncated": 0}, public_frame(b'{"x":1}'))
        self.assertEqual({"encoding": "base64", "value": "/wA=", "truncated": 0}, public_frame(b"\xff\x00"))

    def test_sbe_wire_shape_reads_only_the_standard_header(self) -> None:
        self.assertEqual(
            {"wire": "sbe_binary", "bytes": 8, "block_length": 16,
             "template_id": 1003, "schema_id": 1, "version": 3},
            wire_shape("sbe_binary", bytes((16, 0, 235, 3, 1, 0, 3, 0)), None))

    def test_static_audit_finds_all_declared_core_anchors(self) -> None:
        result = catalog_audit.audit(PRODUCTS)
        self.assertGreaterEqual(result["core_aligned_ws_profiles"], 1)
        self.assertEqual(1, result["anchors_ok"])

    def test_standalone_catalog_does_not_require_a_companion_source_tree(self) -> None:
        result = catalog_audit.audit(PRODUCTS, None)
        self.assertEqual(0, result["source_root_available"])
        self.assertEqual([], result["missing_profile_families"])
        self.assertIsNone(result["anchors_ok"])

    def test_sandbox_rejects_private_or_credential_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            path.write_text('{"venue":"x","product":"spot","private_rest":[]}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "forbidden"):
                sandbox.load(path)

    def test_data_implies_ack_profiles_require_market_data_shape(self) -> None:
        case = next(case for product in PRODUCTS
                    if product.venue == "bitmart" and product.product == "spot"
                    for case in product.public_ws if case.name == "trades")
        self.assertFalse(_matches_data(case, {"event": "subscribe"}, 0x1, b"{}"))
        self.assertTrue(_matches_data(case, {"data": []}, 0x1, b'{"data":[]}'))

    def test_htx_application_ping_is_replied_to_at_json_layer(self) -> None:
        case = WsCase("trades", "example.test", "/", application_heartbeat="htx")
        self.assertEqual(b'{"pong":42}', _application_pong(case, {"ping": 42}))
        self.assertEqual(b"", _application_pong(case, {"status": "ok"}))


if __name__ == "__main__":
    unittest.main()
