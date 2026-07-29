import {describe, expect, it} from "vitest";
import {filterCatalog, receiveLagNs} from "./main";

describe("research viewer helpers", () => {
  it("keeps receive clocks independent", () => {
    const base = {
      event_id: 1,
      channel_id: "depth",
      event_kind: "depth",
      monotonic_ns: 100,
      exchange_event_ns: 50,
      price: "",
      bid_price: "100",
      ask_price: "101",
    };
    expect(receiveLagNs(base, {...base, monotonic_ns: 60_000_100})).toBe(60_000_000);
  });

  it("searches catalog without a server-side database", () => {
    const rows = [{index: 0, venue: "binance", product: "futures", symbol: "BTCUSDT", status: "complete", path: "run"}];
    expect(filterCatalog(rows, "btc")).toHaveLength(1);
    expect(filterCatalog(rows, "okx")).toHaveLength(0);
  });
});
