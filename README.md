# Ultra Low-Latency Market Data Processing Engine

#### C++20 • Non-Blocking TCP • Order Book Reconstruction • Market Microstructure

This project implements a real-time market data replay & order book engine designed for HFT-style workloads.
It includes:

    • A streaming client that replays raw MBO data over TCP at 100k–500k msgs/sec

    • A high-performance engine that parses events, updates the order book, logs metrics, and exposes HTTP endpoints

    • Microsecond-level latency instrumentation (internal vs. end-to-end)

    • Real-time CSV metrics for spread, midprice, depth, and throughput

    • A clean modular C++20 architecture with GoogleTest unit tests

This project simulates the core of an HFT market data pipeline: feed handler → order book → analytics → monitoring.

## Features Overview
### High Throughput Ingestion

    • Replays 250k–500k events per second

    • Zero-copy parsing & minimal dynamic allocation

    • Non-blocking TCP + batched sendmmsg style streaming

### Full Order Book Reconstruction

    • Supports ADD / MOD / CXL / TRD / CLR events with:

    • Price-level aggregation

    • Top-of-book & multi-level depth

    • Trade reporting & deletion semantics

### Microsecond Latency Metrics

Two kinds of latency:

    • Internal latency: (parse → apply)

        mean = 2 μs, p50 = 2 μs, p95 ≈ 5 μs, p99 ≈ 10–14 μs

    • End-to-End latency: (streamer → engine → processed)

        mean = 3–8 ms, p99 < 20 ms at 500k events/sec

### CSV Metrics Output

    • Top-of-book snapshots

    • Spread, midprice

    • Depth levels

    • Events/sec throughput @ 10 Hz

### Real-Time HTTP API

    • GET /health – liveness

    • GET /book/top?n=5 – top levels of the book

    • GET /stats – latency metrics (mean, p50, p95, p99)

## Project Structure
```
batonics_trading_challenge/
│
├── src/
│   ├── common/              # Shared utilities & types
│   ├── engine/              # Core order book engine
│   │   ├── engine.cpp
│   │   ├── order_book.cpp
│   │   ├── parser.cpp
│   │   └── lat_stats.cpp
│   ├── streamer/            # TCP replay client
│   └── http/                # Lightweight HTTP server wrapper
│
├── include/
│   ├── common/
│   ├── engine/
│   ├── streamer/
│   └── http/
│
├── tests/                   # GoogleTest suite for book semantics
├── data/                    # Input & output CSV files
├── CMakeLists.txt
└── README.md                # (this file)
```

## Architecture Overview
```
                ┌───────────────┐
                │   CLX5 file   │
                │ (raw MBO L2)  │
                └───────┬───────┘
                        │ replay @ 100k–500k msg/s
                        ▼
                ┌───────────────┐
                │  StreamerApp  │
                │ Non-block TCP │
                └───────┬───────┘
                        │ send bytes
                        ▼
      ┌──────────────────────────────────────┐
      │              EngineApp               │
      │------------------------------------- │
      │  Parser → MboEvent → OrderBook →     │
      │      Metrics → HTTP → CSV logs       │
      └──────────────────────────────────────┘
                       │
             ┌─────────┴──────────┐
             ▼                    ▼
      metrics.csv         metrics_throughput.csv
```

The engine is intentionally single-threaded to mimic real-world colocated feedhandler logic: deterministic, ultra-low-latency, no locks.

## Detailed File & Method Documentation
### STREAMER LAYER
`streamer_app.cpp`

    • Opens TCP connection to the engine

    • Reads CLX5 text file line-by-line

    • Replays events at a configured rate (lines/sec)

    • Sends each line in a preallocated buffer to avoid allocations

    • Uses non-blocking I/O + send() batching pattern

#### Key methods:
```
**Method**	                        **Purpose**
load_file_lines(filepath)	Reads entire dataset into memory upfront
throttle_send(loop_rate_hz)	Sends messages at target rate (100k–500k/sec)
send_line(sock, line)	        Raw non-blocking send() of bytes
```

## ENGINE LAYER
`EngineApp`

Main loop that receives bytes → parses events → updates book → logs metrics → handles HTTP.

**Key responsibilities:**

    • Manage TCP listener

    • Maintain order book

    • Parse incoming events

    • Track latency

    • Emit CSV logs

    • Serve HTTP endpoints

#### Important methods:
```
**Method**	                        **Description**
start_tcp_listener(port)	Non-blocking accept loop
on_raw_message(buf)	        Called each time a line arrives
on_event(const MboEvent&)	Applies event to the book
emit_csv_snapshot()	        Writes spread/mid/depth to metrics.csv
emit_throughput()	        Writes event/sec to metrics_throughput.csv
dump_latency_stats(os)	        Computes mean, p50, p95, p99
```

## PARSER LAYER
`parser.cpp`

Converts a raw text line into an MboEvent.

Example input:
    `1758742436824419441,ADD,B,64830000000,132,order123`

Processes:

    • timestamp

    • event type

    • side (B/A)

    • price

    • quantity

    • order ID

#### Major methods:
```
**Method**	                                **Purpose**
parse_line(const std::string&, MboEvent&)	Very fast zero-allocation parsing
to_side(char)	                                Maps 'B' → Side::Bid, 'A' → Side::Ask
```

## ORDER BOOK LAYER
`order_book.hpp` / `order_book.cpp`

Maintains book state with price → aggregate size mapping for both BID and ASK.

#### Data structures:

    std::map<int64_t, uint64_t> for bids

    std::map<int64_t, uint64_t> for asks

(Could be replaced with skiplist or flat hash map for production speed.)

#### Methods:
```
**Method**	**Description**
on_add(ev)	Insert new order, increase depth
on_mod(ev)	Update quantity
on_cancel(ev)	Remove order
on_trade(ev)	Reduce size at price (match)
on_clear()	Full wipe (auction / session reset)
best_bid()	Returns highest price level
best_ask()	Returns lowest price level
depth(n)	Aggregates n price levels
```

## LATENCY MEASUREMENT
`lat_stats.cpp`

Two independent histograms:
```
**Histogram**	**Tracks**
internal_us	parse → apply latency
e2e_us	        streamer timestamp → processed timestamp
```

#### Methods:
```
**Method**	        **Function**
record_internal_us(dt)	Add sample
record_e2e_us(dt)	Add sample
compute_percentile(p)	returns p50/p95/p99
dump(os)	        formatting for /stats
```

## HTTP SERVER
A lightweight wrapper around `cpp-httplib`.

Endpoints:

`/health`

Basic heartbeat → `"OK"`

`/book/top?n=N`

Returns:
```
{
  "best_bid_px": 64830000000,
  "best_ask_px": 64820000000,
  "depth_b": [...],
  "depth_a": [...]
}
```

`/stats`

Human-readable latency stats:
```
[latency_us_internal] samples=14959 mean=2 p50=2 p95=5 p99=10
[latency_us_e2e]      samples=14959 mean=2893 p50=4764 p95=4957 p99=5726
```

## UNIT TESTS

Located in `tests/`.

Covers:

• ADD / MODIFY semantics

• CANCEL removes size

• TRADES reduce quantity

• CLEAR resets book

• Best bid / ask correctness

• Multi-level depth aggregation

## Benchmark Results
Internal Latency (parse → apply)

• **mean**: 2 μs

• **p50**: 2 μs

• **p95**: 4–5 μs

• **p99**: 10–14 μs

End-to-End Latency (streamer → engine)

At high load (250k–500k msg/sec):

• **mean**: 3–8 ms

• **p99**: < 20 ms

Throughput

• Sustainably processed **250,000–500,000** events/sec

• Replayed **14,959** events per dataset with no drops

• Detected throughput in CSV logs: **~15k events in 100ms** windows, scaling linearly

## Running the Project
**1. Build**
```
cmake -B build -S .
cmake --build build --config Release
```
**2. Start Engine**
```
./build/bin/Release/streamer_app.exe 9001 ./data/CLX5_lines.txt 250000
```
**3. Start Streamer**
```
./build/bin/Release/streamer_app.exe 9001 ./data/CLX5_lines.txt 250000
```
**4. Monitor**
```
curl http://127.0.0.1:18081/stats
curl http://127.0.0.1:18081/book/top?n=5
Get-Content data/metrics.csv -Wait
```