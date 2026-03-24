# Edge.js Benchmarks

This directory contains small standalone benchmark workloads for comparing Edge.js against other runtimes.

The workload files define the benchmark scenarios.
Benchmark execution and comparison output are handled via [`hyperfine`](https://github.com/sharkdp/hyperfine).

## Goals

- Keep workloads simple and reproducible
- Keep benchmark scenarios separate from the comparison runner
- Measure whole-process wall-clock time for one-shot workloads
- Make results easier to compare and export as JSON / CSV / Markdown

## Current benchmarks

- `empty-startup`: startup cost for an empty script
- `console-log`: startup + trivial execution + stdout handling

## Build Edge locally

```bash
make build