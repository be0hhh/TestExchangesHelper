# Offline tests

Cases are grouped by subject: `Paired`, `Rest`, `Ws`, `Reconnect`, `Contracts`.

Add a case to the matching subject and register its source in this directory's
`CMakeLists.txt`. Keep existing suite targets, case IDs, labels and `EXPECTED`
counts stable when moving cases; update `EXPECTED` when adding coverage.
Use local fixture headers only for shared setup; keep fixture ownership and
lifetimes explicit. Case `.ipp` fragments are included by their suite driver
and must not be compiled separately. These suites stay offline.

`Reference/HttpFrequencyCases.cpp` adds four static-fixture cases for bounded
reference HTTP ladder CLI selection, confirmation versus distinct-update
aggregation and response latency quantiles, fail-stop decisions, and artifact
capacity/redaction. They use numeric synthetic observations only and do not
start a source or perform network requests.
