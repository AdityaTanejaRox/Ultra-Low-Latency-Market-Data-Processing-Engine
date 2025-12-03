#include "engine/engine.hpp"
#include "common/net.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <limits>
#include <cmath>

namespace engine
{

    using namespace std::chrono;

    uint64_t engine::EngineApp::now_ns()
    {
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }

    static inline uint64_t wall_ns()
    {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    }


    void EngineApp::start_throughput_thread()
    {
        if (!csv_enabled_) return;

        if (!thr_csv_.is_open())
        {
            thr_csv_.open(throughput_path_, std::ios::out | std::ios::trunc);
            thr_csv_ << "ts_ns,events_per_sec\n";
            std::cout << "[engine] throughput CSV -> " << throughput_path_ << "\n";
        }
        thr_stop_ = false;
        thr_thread_ = std::thread([this]
        {
            using namespace std::chrono;
            const auto period = 50ms;              // sample every 50 ms
            const double scale = 1000.0 / 50.0;    // 20x to convert to per-second

            while (!thr_stop_.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(period);
                uint64_t delta = applied_since_tick_.exchange(0, std::memory_order_relaxed);
                // scale to events/sec equivalent
                uint64_t eps = static_cast<uint64_t>(std::llround(delta * scale));
                if (thr_csv_.is_open())
                {
                    thr_csv_ << wall_ns() << "," << eps << "\n";
                    thr_csv_.flush();
                }
            }
        });
    }


    void engine::EngineApp::stop_throughput_thread()
    {
        thr_stop_ = true;
        if (thr_thread_.joinable()) thr_thread_.join();
        if (thr_csv_.is_open()) thr_csv_.flush();
    }

    void EngineApp::record_latency_us(uint64_t us)
    {
        int bin = (int)(us / LAT_BIN_US);
        if (bin > LAT_BINS) bin = LAT_BINS;
        lat_bins_[bin].fetch_add(1, std::memory_order_relaxed);
        lat_samples_.fetch_add(1, std::memory_order_relaxed);
        lat_sum_us_.fetch_add(us, std::memory_order_relaxed);
    }

    void EngineApp::record_e2e_latency_us(uint64_t us)
    {
        int bin = (int)(us / E2E_BIN_US);
        if (bin > E2E_BINS) bin = E2E_BINS;
        e2e_bins_[bin].fetch_add(1, std::memory_order_relaxed);
        e2e_samples_.fetch_add(1, std::memory_order_relaxed);
        e2e_sum_us_.fetch_add(us, std::memory_order_relaxed);
    }

    void EngineApp::dump_latency_stats(std::ostream& os)
    {
        // Internal latency stats (parse -> apply, steady clock)
        uint64_t total = lat_samples_.load(std::memory_order_relaxed);
        if (total == 0)
        {
            os << "[latency_us] no samples\n";
        }
        else
        {
            auto quant = [&](double p)->uint64_t
            {
                uint64_t need = (uint64_t)std::ceil(p * total);
                uint64_t acc = 0;
                for (int i = 0; i <= LAT_BINS; ++i)
                {
                    acc += lat_bins_[i].load(std::memory_order_relaxed);
                    if (acc >= need) return (uint64_t)i * LAT_BIN_US;
                }
                return (uint64_t)LAT_BINS * LAT_BIN_US;
            };
            uint64_t mean = lat_sum_us_.load(std::memory_order_relaxed) / total;
            os << "[latency_us_internal] samples=" << total
            << " mean=" << mean
            << " p50="  << quant(0.50)
            << " p95="  << quant(0.95)
            << " p99="  << quant(0.99)
            << " (bin=" << LAT_BIN_US << "us)\n";
        }

        // E2E latency stats (producer -> consumer, wall clock)
        uint64_t e2e_total = e2e_samples_.load(std::memory_order_relaxed);
        if (e2e_total == 0)
        {
            os << "[latency_us_e2e] no samples\n";
        }
        else
        {
            auto quantE = [&](double p)->uint64_t
            {
                uint64_t need = (uint64_t)std::ceil(p * e2e_total);
                uint64_t acc = 0;
                for (int i = 0; i <= E2E_BINS; ++i)
                {
                    acc += e2e_bins_[i].load(std::memory_order_relaxed);
                    if (acc >= need) return (uint64_t)i * E2E_BIN_US;
                }
                return (uint64_t)E2E_BINS * E2E_BIN_US;
            };
            uint64_t meanE = e2e_sum_us_.load(std::memory_order_relaxed) / e2e_total;
            os << "[latency_us_e2e] samples=" << e2e_total
            << " mean=" << meanE
            << " p50="  << quantE(0.50)
            << " p95="  << quantE(0.95)
            << " p99="  << quantE(0.99)
            << " (bin=" << E2E_BIN_US << "us)\n";
        }
    }

    void EngineApp::enable_json_snapshots(const std::string& path)
    {
        json_snapshots_.open(path, std::ios::out | std::ios::trunc);
        if (!json_snapshots_)
        {
            std::cerr << "[engine] failed to open JSON snapshots file: " << path << "\n";
            return;
        }
        json_enabled_ = true;
    }

    void EngineApp::write_snapshot_json(std::int64_t ts_ns)
    {
        if (!json_enabled_ || !json_snapshots_.is_open()) return;

        // grab a snapshot of the book – using snapshot_top_n()
        auto snap = book_.snapshot_full();

        json_snapshots_ << "{";
        json_snapshots_ << "\"ts_ns\":" << ts_ns << ",\"bids\":[";
        for (std::size_t i = 0; i < snap.bids.size(); ++i) {
            const auto& lvl = snap.bids[i];
            if (i) json_snapshots_ << ',';
            json_snapshots_ << "{\"px\":" << lvl.price
                            << ",\"qty\":" << lvl.total_qty
                            << ",\"orders\":" << lvl.orders
                            << "}";
        }
        json_snapshots_ << "],\"asks\":[";
        for (std::size_t i = 0; i < snap.asks.size(); ++i) {
            const auto& lvl = snap.asks[i];
            if (i) json_snapshots_ << ',';
            json_snapshots_ << "{\"px\":" << lvl.price
                            << ",\"qty\":" << lvl.total_qty
                            << ",\"orders\":" << lvl.orders
                            << "}";
        }
        json_snapshots_ << "]}\n";
    }


    static std::vector<std::string> split_csv(const std::string& s)
    {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            out.push_back(item);
        }
        return out;
    }

    void EngineApp::enable_csv_metrics(const std::string& path, size_t every)
    {
        csv_path_ = path;
        log_every_ = (every ? every : 1000);
        csv_enabled_ = true;

        // derive throughput csv path
        throughput_path_ = csv_path_;
        auto pos = throughput_path_.rfind(".csv");
        if (pos != std::string::npos)
        {
            throughput_path_.insert(pos, "_throughput");
        }
        else
        {
            throughput_path_ += "_throughput.csv";
        }
    }

    BookSnapshot EngineApp::snapshot_top_n_locked(size_t n)
    {
        std::lock_guard<std::mutex> lg(mtx_);
        return book_.snapshot_top_n(n);
    }

    void EngineApp::maybe_log_csv(uint64_t ts_ns)
    {
        if (!csv_enabled_) return;
        uint64_t c = ++ev_count_;
        if ((c % log_every_) != 0) return;

        if (!csv_.is_open())
        {
            csv_.open(csv_path_, std::ios::out | std::ios::trunc);
        }
        if (!csv_header_written_)
        {
            csv_ << "ts_ns,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty,spread,mid,depth_b,depth_a\n";
            csv_header_written_ = true;
        }

        // Take a small snapshot for metrics
        BookSnapshot s = snapshot_top_n_locked(1);

        long long bid_px = std::numeric_limits<long long>::min();
        long long ask_px = std::numeric_limits<long long>::max();
        long long bid_qty = 0, ask_qty = 0;
        long long depth_b = 0, depth_a = 0; // top-level order counts

        if (!s.bids.empty())
        {
            bid_px  = s.bids[0].price;
            bid_qty = s.bids[0].total_qty;
            depth_b = s.bids[0].orders;
        }
        if (!s.asks.empty())
        {
            ask_px  = s.asks[0].price;
            ask_qty = s.asks[0].total_qty;
            depth_a = s.asks[0].orders;
        }

        long long spread = (ask_px == std::numeric_limits<long long>::max() || bid_px == std::numeric_limits<long long>::min())
                            ? -1 : (ask_px - bid_px);
        double mid = (spread < 0) ? NAN : ( (double)ask_px + (double)bid_px ) * 0.5;

        csv_ << ts_ns << "," << bid_px << "," << bid_qty << ","
            << ask_px << "," << ask_qty << ","
            << spread << "," << mid << ","
            << depth_b << "," << depth_a << "\n";

        csv_.flush();
    }


    void EngineApp::handle_line(const std::string& line)
    {
        if (line.empty()) return;

        // Optional end-to-end stamp: prefix is "@<send_wall_ns>,"
        uint64_t send_wall_ns = 0;
        size_t start_pos = 0;
        if (!line.empty() && line[0] == '@')
        {
            // find first comma
            size_t comma = line.find(',', 1);
            if (comma != std::string::npos)
            {
                // parse the number between '@' and ','
                try
                {
                    send_wall_ns = std::stoull(line.substr(1, comma - 1));
                    start_pos = comma + 1; // skip past the comma; remaining is the original CSV
                }
                catch (...)
                {
                    // if parsing fails, just ignore the E2E stamp
                    send_wall_ns = 0;
                    start_pos = 0;
                }
            }
        }


         // mark receive
        uint64_t t_recv_ns = now_ns();

        auto fields = split_csv(line.substr(start_pos));
        if (fields.empty()) return;

        MboEvent ev{};
        const std::string& kind = fields[0];

        try
        {
            if (kind == "ADD" && fields.size() >= 6)
            {
                ev.kind = EventKind::Add;
                // fields: ADD, ts_ns, side, order_id, price, qty
                ev.ts_ns   = std::stoull(fields[1]);
                ev.side    = (fields[2] == "B") ? Side::Bid : Side::Ask;
                ev.order_id= std::stoull(fields[3]);
                ev.price   = std::stoll(fields[4]);
                ev.qty     = std::stoi(fields[5]);
            }
            else if (kind == "MOD" && fields.size() >= 5)
            {
                ev.kind = EventKind::Modify;
                // MOD, ts_ns, order_id, new_price, new_qty
                ev.ts_ns   = std::stoull(fields[1]);
                ev.order_id= std::stoull(fields[2]);
                ev.new_price = std::stoll(fields[3]);
                ev.new_qty = std::stoi(fields[4]);
            }
            else if (kind == "CXL" && fields.size() >= 3)
            {
                ev.kind = EventKind::Cancel;
                // CXL, ts_ns, order_id
                ev.ts_ns   = std::stoull(fields[1]);
                ev.order_id= std::stoull(fields[2]);
            }
            else if (kind == "TRD" && fields.size() >= 4)
            {
                ev.kind = EventKind::Trade;
                // TRD, ts_ns, order_id, fill_qty
                ev.ts_ns   = std::stoull(fields[1]);
                ev.order_id= std::stoull(fields[2]);
                ev.qty     = std::stoi(fields[3]);
            }
            else if (kind == "CLR" && fields.size() >= 2)
            {
                ev.kind = EventKind::Clear;
                ev.ts_ns = std::stoull(fields[1]);
            }
            else
            {
                // unknown line; ignore for now
                return;
            }
        }
        catch (...)
        {
            // parse error; ignore line for now
            return;
        }

        {
            std::lock_guard<std::mutex> lg(mtx_);
            book_.on_event(ev);
            
            // But this would slow everything down....
            if (json_enabled_)
            {
                write_snapshot_json(ev.ts_ns);
            }
        }

        

        // End-to-end latency: consumer apply time vs producer send wall-clock
        if (send_wall_ns != 0)
        {
            // use system_clock 'now' for wall time compatibility with streamer
            uint64_t apply_wall_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
            if (apply_wall_ns > send_wall_ns)
            {
                uint64_t e2e_us = (apply_wall_ns - send_wall_ns) / 1000ULL;
                record_e2e_latency_us(e2e_us);
            }
        }


        // latency: parse-to-apply in microseconds
        uint64_t t_apply_ns = now_ns();
        uint64_t lat_us = (t_apply_ns - t_recv_ns) / 1000ULL;
        record_latency_us(lat_us);
        applied_since_tick_.fetch_add(1, std::memory_order_relaxed);

        // write CSV every K events using ts_ns of this event
        maybe_log_csv(ev.ts_ns);
    }

    void EngineApp::print_snapshot(size_t top_n)
    {
        auto s = book_.snapshot_top_n(top_n);
        auto print_side = [](const char* name, const std::vector<LevelView>& lv)
        {
            std::cout << name << ":";
            for (auto& x : lv)
            {
                std::cout << " [" << x.price << " x " << x.total_qty << " (" << x.orders << ")]";
            }
            std::cout << "\n";
        };
        print_side("BIDS", s.bids);
        print_side("ASKS", s.asks);
    }

    void EngineApp::run_http_server(EngineApp* self, int port)
    {
        httplib::Server srv;

        // /health returns 200 quickly
        srv.Get("/health", [](const httplib::Request&, httplib::Response& res)
        {
            res.set_content("{\"ok\":true}", "application/json");
        });

        // existing /book/top endpoint (unchanged)
        srv.Get("/book/top", [self](const httplib::Request& req, httplib::Response& res)
        {
            size_t n = 5;
            if (auto it = req.params.find("n"); it != req.params.end()) {
            try { n = static_cast<size_t>(std::stoul(it->second)); } catch (...) {}
            }
            auto snap = self->snapshot_top_n_locked(n);

            std::ostringstream out;
            out << "{";
            auto dump_side = [&](const char* name, const std::vector<LevelView>& lv)
            {
                out << "\"" << name << "\":[";
                for (size_t i = 0; i < lv.size(); ++i)
                {
                    const auto& x = lv[i];
                    out << "{\"price\":" << x.price
                        << ",\"qty\":" << x.total_qty
                        << ",\"orders\":" << x.orders
                        << "}";
                    if (i + 1 < lv.size()) out << ",";
                }
                out << "]";
            };
            dump_side("bids", snap.bids); out << ","; dump_side("asks", snap.asks); out << "}";
            res.set_content(out.str(), "application/json");
        });

        srv.Get("/spread", [self](const httplib::Request&, httplib::Response& res)
        {
            auto snap = self->snapshot_top_n_locked(1);
            long long bid = snap.bids.empty() ? LLONG_MIN : snap.bids[0].price;
            long long ask = snap.asks.empty() ? LLONG_MAX : snap.asks[0].price;
            long long spread = (bid == LLONG_MIN || ask == LLONG_MAX) ? -1 : (ask - bid);
            std::ostringstream out;
            out << "{\"bid\":" << (bid==LLONG_MIN? -1:bid)
                << ",\"ask\":" << (ask==LLONG_MAX? -1:ask)
                << ",\"spread\":" << spread << "}";
            res.set_content(out.str(), "application/json");
        });

        srv.Get("/stats", [self](const httplib::Request&, httplib::Response& res)
        {
            std::ostringstream os;
            self->dump_latency_stats(os);
            res.set_content(os.str(), "text/plain");
        });

        // log each request to stdout
        srv.set_logger([](const auto& req, const auto& res)
        {
            std::cout << "[http] " << req.method << " " << req.path << " -> " << res.status << "\n";
        });

        std::cout << "[engine] HTTP listening on http://127.0.0.1:" << port << "/health\n";
        std::cout << "[engine] HTTP listening on http://127.0.0.1:" << port << "/book/top?n=5\n";

        // bind to loopback and a non-8080 port
        srv.listen("127.0.0.1", port);
    }

    int EngineApp::run(const std::string& host, const std::string& port, size_t top_n)
    {
        default_top_n_ = top_n;

        // fire an HTTP server on port 18081
        std::thread http_thr(run_http_server, this, 18081);
        http_thr.detach();

        // start throughput thread if metrics enabled
        start_throughput_thread();

        int lfd = net::listen_tcp(host, port);
        std::cout << "[engine] listening on " << host << ":" << port << "\n";

        for (;;)
        {
            // accept loop
            int cfd = net::accept_one(lfd);
            std::cout << "[engine] client connected\n";
            net::set_nonblocking(cfd, true);

            std::string buf; buf.reserve(1<<20);
            std::string line; line.reserve(4096);
            std::vector<char> chunk(64 * 1024); // 64KB read buffer

            while (true)
            {
                // wait until we can read (or timeout)
                if (!net::wait_readable(cfd, /*timeout_ms*/ 1000))
                {
                    continue; // nothing this tick
                }

                size_t n = net::recv_some(cfd, chunk.data(), chunk.size());
                if (n == SIZE_MAX)
                {
                    // would-block race; try again
                    continue;
                }
                if (n == 0)
                {
                    // peer closed
                    break;
                }
                buf.append(chunk.data(), chunk.data() + n);

                size_t pos = 0;
                while (true)
                {
                    auto nl = buf.find('\n', pos);
                    if (nl == std::string::npos)
                    {
                        buf.erase(0, pos);
                        break;
                    }
                    line.assign(buf.data() + pos, nl - pos);
                    pos = nl + 1;
                    handle_line(line);
                    // static uint64_t ctr = 0;
                    // if ((++ctr % 10000) == 0) print_snapshot(top_n);
                }
            }

            net::close_fd(cfd);
            std::cout << "[engine] client disconnected\n";
        }

        // (unreachable in this simple loop)
        net::close_fd(lfd);
        std::cout << "[engine] client disconnected\n";
        return 0;
    }

} // namespace engine
