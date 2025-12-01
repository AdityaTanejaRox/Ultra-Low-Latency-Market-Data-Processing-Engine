#pragma once
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include <utility>

namespace net
{

    int  listen_tcp(std::string_view host, std::string_view port, int backlog = 128);
    int  accept_one(int listen_fd);
    int  connect_tcp(std::string_view host, std::string_view port);
    void send_all(int fd, const void* data, size_t len);
    size_t recv_some(int fd, void* buf, size_t cap);
    void close_fd(int fd);

    void set_nonblocking(int fd, bool nb);

    bool wait_readable(int fd, int timeout_ms);
    bool wait_writable(int fd, int timeout_ms);


    // Returns total bytes sent/received; may be less than sum of iov lens.
    struct IoVec { void* base; size_t len; };
    size_t recvv(int fd, IoVec* vecs, int count);
    size_t sendv(int fd, const IoVec* vecs, int count);

    // (Linux only): batch syscalls for lower syscall overhead
    // Returns number of datagrams processed, not bytes (0 => EAGAIN/timeout)
    int recvmmsg_batch(int fd, void** bufs, size_t* lens, int count);
    int sendmmsg_batch(int fd, const void** bufs, const size_t* lens, int count);

    // (Linux only): enable kernel zero-copy for large sends (MSG_ZEROCOPY)
    void enable_zerocopy(int fd, bool on);

} // namespace net
