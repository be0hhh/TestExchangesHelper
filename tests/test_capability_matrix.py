from __future__ import annotations

import sys
import unittest
from pathlib import Path


TOOL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_ROOT))

import catalog_audit  # noqa: E402
from redaction import shape  # noqa: E402
from venues import PRODUCTS  # noqa: E402


PUBLIC_CAPABILITIES = frozenset({
    "exchange_info",
    "instrument_catalog",
    "instrument_detail",
    "funding_current_all",
    "funding_current_symbol",
    "funding_history",
    "historical_trades",
    "live_trades",
    "live_bbo",
    "live_l2",
})

PRIVATE_CAPABILITIES = frozenset({
    "balances",
    "fills_history",
    "positions",
    "open_orders",
    "order_history",
    "user_account_stream",
    "user_orders_stream",
    "user_trades_stream",
})

MUTATING_NAME_PARTS = (
    "place",
    "submit",
    "cancel",
    "amend",
    "replace",
    "create_order",
    "close_position",
)


class CapabilityMatrixContractTests(unittest.TestCase):
    @staticmethod
    def _capabilities():
        return [capability for product in PRODUCTS for capability in product.capabilities]

    def test_capability_inventory_covers_requested_public_and_private_classes(self) -> None:
        capabilities = self._capabilities()
        public_names = {capability.name for capability in capabilities if capability.surface == "public"}
        private_names = {capability.name for capability in capabilities if capability.surface == "private"}

        self.assertTrue(PUBLIC_CAPABILITIES <= public_names)
        self.assertTrue(PRIVATE_CAPABILITIES <= private_names)
        for capability in capabilities:
            self.assertIn(capability.surface, {"public", "private"})
            self.assertEqual(capability.private, capability.surface == "private")

    def test_probe_matrix_is_read_only(self) -> None:
        for capability in self._capabilities():
            self.assertFalse(capability.mutation, msg=capability.name)
            self.assertFalse(
                any(part in capability.name.lower() for part in MUTATING_NAME_PARTS),
                msg=capability.name,
            )

    def test_private_cases_require_confirmation_and_redaction(self) -> None:
        private_capabilities = [
            capability for capability in self._capabilities() if capability.surface == "private"
        ]
        self.assertGreater(len(private_capabilities), 0)
        for capability in private_capabilities:
            self.assertTrue(capability.requires_confirmation, msg=capability.name)

        redacted = shape({"api_key": "must-not-appear", "balance": 123})
        self.assertEqual("<redacted>", redacted["api_key"])
        self.assertEqual("int", redacted["balance"])

    def test_wire_lanes_explicitly_mark_selected_and_diagnostic_variants(self) -> None:
        market_data = [
            capability for capability in self._capabilities()
            if capability.surface == "public"
            and capability.name in {"live_trades", "live_bbo", "live_l2"}
        ]
        self.assertGreater(len(market_data), 0)
        self.assertTrue(all(capability.transport for capability in market_data))
        self.assertTrue(all(capability.wire for capability in market_data))
        self.assertTrue(any(capability.selection == "core_selected" for capability in market_data))
        self.assertTrue(any(capability.selection == "diagnostic_variant" for capability in market_data))

    def test_finam_families_are_excluded_from_the_python_probe(self) -> None:
        venues = {product.venue for product in PRODUCTS}
        self.assertNotIn("finam", venues)
        self.assertNotIn("finam_arena", venues)

        result = catalog_audit.audit(PRODUCTS)
        self.assertIn("finam", result["excluded_families"])
        self.assertIn("finam_arena", result["excluded_families"])


if __name__ == "__main__":
    unittest.main()
