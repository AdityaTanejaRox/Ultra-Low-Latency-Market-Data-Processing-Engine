#pragma once
#include "engine/order_book.hpp"
#include <string>
#include <mutex>
#include <fstream>
#include <atomic>
#include <thread>
#include <atomic>
#include <functional>
#include "httplib.h"

namespace engine
{

    // Engine: reads newline-delimited frames from a TCP socket and applies to the book.
    //
    //  ADD,<ts_ns>,<side>,<order_id>,<price_ticks>,<qty>
    //  MOD,<ts_ns>,<order_id>,<new_price_ticks>,<new_qty>
    //  CXL,<ts_ns>,<order_id>
    //  TRD,<ts_ns>,<order_id>,<fill_qty>
    //  CLR,<ts_ns>
    // side: B or A
    //
    class EngineApp
    {
    public:
        int run(const std::string& host, const std::string& port, size_t top_n);
        void enable_csv_metrics(const std::string& path, size_t every);
        static void run_http_server(EngineApp* self, int port);
    private:

        OrderBook book_;

        // metrics (book snapshot)
        std::mutex mtx_;
        std::ofstream csv_;
        std::string csv_path_;
        size_t log_every_ = 1000;
        std::atomic<uint64_t> ev_count_{0};
        bool csv_enabled_ = false;
        bool csv_header_written_ = false;
        size_t default_top_n_ = 5;

        // throughput (events/sec)
        std::string throughput_path_;
        std::ofstream thr_csv_;
        std::thread thr_thread_;
        std::atomic<bool> thr_stop_{false};
        std::atomic<uint64_t> applied_since_tick_{0};

        // latency histogram (µs)
        static constexpr int LAT_BIN_US = 1;     // 1 µs bins
        static constexpr int LAT_BINS   = 5000;  // cover 0..5 ms; last bin = overflow
        std::atomic<uint64_t> lat_bins_[LAT_BINS + 1]{};
        std::atomic<uint64_t> lat_samples_{0};
        std::atomic<uint64_t> lat_sum_us_{0};   // sum of latencies for mean

        // E2E latency (producer→consumer) histogram in µs
        static constexpr int E2E_BIN_US = 1;     // 1 µs bins
        static constexpr int E2E_BINS   = 100000; // cover 0..100 ms; last bin overflow
        std::atomic<uint64_t> e2e_bins_[E2E_BINS + 1]{};
        std::atomic<uint64_t> e2e_samples_{0};
        std::atomic<uint64_t> e2e_sum_us_{0};

        // helpers
        void record_e2e_latency_us(uint64_t us);
        void handle_line(const std::string& line);
        void print_snapshot(size_t top_n);
        BookSnapshot snapshot_top_n_locked(size_t n);
        
        void maybe_log_csv(uint64_t ts_ns);

        // throughput thread and helpers
        void start_throughput_thread();
        void stop_throughput_thread();
        static uint64_t now_ns();

        // latency helpers
        void record_latency_us(uint64_t us);
        void dump_latency_stats(std::ostream& os);
    };

} // namespace engine
