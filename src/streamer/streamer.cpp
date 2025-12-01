#include "streamer/streamer.hpp"
#include "common/net.hpp"

#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

namespace streamer {

// Simple token-bucket rate limiter:
// - target lines_per_sec
// - burst up to batch_size lines at once (we'll use 1024 by default)
struct RateLimiter {
  double rate;            // lines per second
  double tokens = 0.0;
  std::chrono::steady_clock::time_point last;

  explicit RateLimiter(double r)
      : rate(std::max(1.0, r)), last(std::chrono::steady_clock::now()) {}

  // Add tokens according to elapsed time; return how many we can spend now (clamped to 'want').
  size_t grant(size_t want, size_t max_burst)
  {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    tokens = std::min<double>(max_burst, tokens + dt * rate);
    size_t can = static_cast<size_t>(tokens);
    if (can == 0) return 0;
    size_t take = std::min(want, std::min(can, max_burst));
    tokens -= take;
    return take;
  }
};

static inline uint64_t wall_ns()
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}


int Streamer::run(const std::string& host, const std::string& port,
                  const std::string& input_file, size_t lines_per_sec)
{
  // 1) connect + make non-blocking
  int fd = net::connect_tcp(host, port);
  net::set_nonblocking(fd, true);

  // Optional (Linux): enable kernel zero-copy for large sends (no-op elsewhere)
  net::enable_zerocopy(fd, true);

  std::ifstream in(input_file);
  if (!in)
  {
    std::cerr << "[streamer] cannot open " << input_file << "\n";
    net::close_fd(fd);
    return 1;
  }
  std::cout << "[streamer] connected to " << host << ":" << port << "\n";

  // 2) Read file in chunks, batch sends to reduce syscalls
  constexpr size_t kBatchLines = 1024;   // lines per load/burst
  constexpr size_t kMaxLineLen = 4096;   // guardrail for pathological lines

  std::vector<std::string> lines;
  lines.reserve(kBatchLines);

  // Buffers used for batched send APIs:
  std::vector<const void*> ptrs(kBatchLines, nullptr);
  std::vector<size_t>      lens(kBatchLines, 0);

  // For writev/WSASend fallback:
  std::vector<net::IoVec>  iov(kBatchLines);

  RateLimiter rl(static_cast<double>(lines_per_sec > 0 ? lines_per_sec : 100000));
  size_t total_sent_lines = 0;

  // 3) Main loop: read lines, then send ALL of them (rate-limited), not just the first 'allowed'.
  std::string line;
  line.reserve(256);

  while (true)
  {
    lines.clear();

    // Fill up to kBatchLines from file
    for (size_t i = 0; i < kBatchLines && std::getline(in, line); ++i)
    {
      if (line.size() > kMaxLineLen) line.resize(kMaxLineLen);

      // Prefix with wall-clock send timestamp in ns: @<ns>,
      // Example: @1731284001123456789,ADD,17587...,B,123,64830000000,10
      std::string out;
      out.reserve(line.size() + 32);
      out.push_back('@');
      out += std::to_string(wall_ns());
      out.push_back(',');
      out += line;
      out.push_back('\n');

      lines.emplace_back(std::move(out));
      line.clear();
    }
    if (lines.empty()) break; // EOF

    // We must send ALL lines in this batch; we just throttle how fast.
    size_t start = 0; // index into `lines` for what remains in this batch
    while (start < lines.size())
    {
      size_t remaining = lines.size() - start;

      // Next burst (rate-limited). Clamp to kBatchLines and remaining.
      size_t allowed = rl.grant(remaining, kBatchLines);
      while (allowed == 0)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        allowed = rl.grant(remaining, kBatchLines);
      }

      size_t sent = 0;
      while (sent < allowed) {
        size_t chunk = allowed - sent;

        // Prepare ptrs/lens/iov for window [start+sent .. start+sent+chunk)
        for (size_t i = 0; i < chunk; ++i)
        {
          size_t idx = start + sent + i;
          ptrs[i] = lines[idx].data();
          lens[i] = lines[idx].size();
          iov[i]  = net::IoVec{ const_cast<char*>(lines[idx].data()), lines[idx].size() };
        }

#ifdef __linux__
        int rc = net::sendmmsg_batch(fd, ptrs.data(), lens.data(), static_cast<int>(chunk));
        if (rc > 0) {
          sent += static_cast<size_t>(rc);
          total_sent_lines += static_cast<size_t>(rc); // delta
          continue;
        }
        // EAGAIN: wait until socket writable
        if (!net::wait_writable(fd, 1)) continue;
#else
        size_t wrote = net::sendv(fd, iov.data(), static_cast<int>(chunk));
        if (wrote == 0)
        {
          if (!net::wait_writable(fd, 1)) continue;
        }
        else
        {
          // Translate bytes -> whole lines; handle partial on current line
          size_t bytes = wrote;
          size_t adv   = 0;
          while (bytes && adv < chunk)
          {
            size_t L = lens[adv];
            if (bytes >= L)
            {
              bytes -= L;
              ++adv;
            }
            else
            {
              // Partial on this line: adjust iov/ptrs/lens for next loop
              const char* base = static_cast<const char*>(ptrs[adv]);
              ptrs[adv] = base + bytes;
              lens[adv] -= bytes;
              iov[adv].base = const_cast<char*>(static_cast<const char*>(ptrs[adv]));
              iov[adv].len  = lens[adv];
              bytes = 0;
            }
          }
          sent += adv;
          total_sent_lines += adv; // delta
        }
#endif
      } // while (sent < allowed)

      // Advance the batch window by what we successfully sent this throttled step
      start += sent;
    } // while (start < lines.size())
  }   // while read batches

  std::cout << "[streamer] done. lines sent: " << total_sent_lines << "\n";
  net::close_fd(fd);
  return 0;
}

} // namespace streamer
