import "./style.css";

export type CatalogRow = {
  index: number;
  venue: string;
  product: string;
  symbol: string;
  status: string;
  path: string;
};

export type ResearchEvent = {
  event_id: number;
  channel_id: string;
  event_kind: string;
  monotonic_ns: number;
  utc_ns?: number;
  exchange_event_ns: number | null;
  exchange_transaction_ns?: number | null;
  price: string;
  bid_price: string;
  ask_price: string;
};

export function receiveLagNs(
  source: ResearchEvent,
  target: ResearchEvent,
): number {
  return target.monotonic_ns - source.monotonic_ns;
}

export function filterCatalog(
  rows: CatalogRow[],
  query: string,
): CatalogRow[] {
  const normalized = query.trim().toLowerCase();
  if (!normalized) return rows;
  return rows.filter((row) =>
    `${row.venue} ${row.product} ${row.symbol} ${row.path}`
      .toLowerCase()
      .includes(normalized),
  );
}

async function getJson<T>(url: string): Promise<T> {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${response.status} ${url}`);
  return response.json() as Promise<T>;
}

async function getLines<T>(url: string): Promise<T[]> {
  const response = await fetch(url);
  if (!response.ok) return [];
  return (await response.text())
    .split("\n")
    .filter(Boolean)
    .map((line) => JSON.parse(line) as T);
}

function bootViewer(): void {
  const app = document.querySelector<HTMLDivElement>("#app");
  if (!app) return;
  app.innerHTML = `
    <header><h1>Exchange API Probe</h1>
      <input id="search" type="search" placeholder="venue, product, symbol">
    </header>
    <main><aside id="catalog"></aside><section>
      <div id="summary"></div><div id="timeline"></div>
      <h2>Findings</h2><pre id="findings"></pre>
    </section></main>`;
  let catalog: CatalogRow[] = [];

  const selectBundle = async (index: number): Promise<void> => {
    const [manifest, findings, events] = await Promise.all([
      getJson<Record<string, unknown>>(
        `/api/file?bundle=${index}&name=manifest.json`,
      ),
      getJson<Record<string, unknown>>(
        `/api/file?bundle=${index}&name=findings.json`,
      ).catch(() => ({})),
      getLines<ResearchEvent>(
        `/api/preview?bundle=${index}&name=events.jsonl&limit=5000`,
      ),
    ]);
    document.querySelector("#summary")!.textContent =
      `${String(manifest.venue)} / ${String(manifest.product)} / ` +
      `${String(manifest.symbol)} - ${events.length} preview events`;
    document.querySelector("#findings")!.textContent =
      JSON.stringify(findings, null, 2);
    const lanes = new Map<string, number>();
    for (const event of events) {
      lanes.set(event.channel_id, (lanes.get(event.channel_id) ?? 0) + 1);
    }
    document.querySelector("#timeline")!.innerHTML = [...lanes]
      .map(([channel, count]) => `<div><strong>${channel}</strong>: ${count}</div>`)
      .join("");
  };

  const render = (): void => {
    const query =
      document.querySelector<HTMLInputElement>("#search")?.value ?? "";
    document.querySelector("#catalog")!.innerHTML = filterCatalog(catalog, query)
      .map(
        (row) =>
          `<button data-index="${row.index}"><strong>${row.venue} / ` +
          `${row.product}</strong><br>${row.symbol}<br><small>${row.status}</small></button>`,
      )
      .join("");
    document.querySelectorAll<HTMLButtonElement>("#catalog button")
      .forEach((button) => {
        button.onclick = () => {
          void selectBundle(Number(button.dataset.index));
        };
      });
  };

  document.querySelector("#search")?.addEventListener("input", render);
  void getJson<CatalogRow[]>("/api/catalog")
    .then((rows) => {
      catalog = rows;
      render();
      if (rows.length) return selectBundle(rows[0].index);
      return undefined;
    })
    .catch((error: unknown) => {
      document.querySelector("#findings")!.textContent = String(error);
    });
}

if (typeof document !== "undefined") bootViewer();
