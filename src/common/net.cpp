#include "common/net.hpp"
#include <stdexcept>
#include <system_error>
#include <cstring>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  static struct WsaInit
  {
    WsaInit()
    {
        WSADATA w;
        WSAStartup(MAKEWORD(2,2), &w);
    }
    ~WsaInit()
    {
        WSACleanup();
    }
} _wsa_init_;
  using socklen_t = int;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #ifdef __linux__
    #include <sys/uio.h>
    #include <sys/epoll.h>
    #include <linux/net_tstamp.h>
    #include <linux/errqueue.h>
  #endif
#endif

namespace
{

    int make_fd(int family, int socktype, int proto)
    {
        int fd = ::socket(family, socktype, proto);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket()");
        return fd;
    }

    void set_reuse(int fd)
    {
        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    }

    void sys_close(int fd)
    {
        #ifdef _WIN32
        ::closesocket(fd);
        #else
        ::close(fd);
        #endif
    }

} // namespace

namespace net
{

    int listen_tcp(std::string_view host, std::string_view port, int backlog)
    {
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        struct addrinfo* res = nullptr;
        if (int rc = ::getaddrinfo(host.empty()?nullptr:std::string(host).c_str(), std::string(port).c_str(), &hints, &res); rc != 0)
        {
            throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));
        }

        int fd = -1;
        for (auto* p = res; p; p = p->ai_next)
        {
            try
            {
                fd = make_fd(p->ai_family, p->ai_socktype, p->ai_protocol);
                set_reuse(fd);
                if (::bind(fd, p->ai_addr, (socklen_t)p->ai_addrlen) == 0)
                {
                    if (::listen(fd, backlog) == 0) break;
                }
                sys_close(fd);
                fd = -1;
            }
            catch (...)
            {
                if (fd >= 0) sys_close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        if (fd < 0) throw std::runtime_error("listen_tcp: failed to bind/listen");
        return fd;
    }

    int accept_one(int listen_fd)
    {
        struct sockaddr_storage ss{};
        socklen_t slen = sizeof(ss);
        int cfd = ::accept(listen_fd, (struct sockaddr*)&ss, &slen);
        if (cfd < 0) throw std::system_error(errno, std::generic_category(), "accept()");
        return cfd;
    }

    int connect_tcp(std::string_view host, std::string_view port)
    {
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* res = nullptr;
        if (int rc = ::getaddrinfo(std::string(host).c_str(), std::string(port).c_str(), &hints, &res); rc != 0)
        {
            throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));
        }

        int fd = -1;
        for (auto* p = res; p; p = p->ai_next)
        {
            try
            {
                fd = make_fd(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (::connect(fd, p->ai_addr, (socklen_t)p->ai_addrlen) == 0) break;
                sys_close(fd);
                fd = -1;
            }
            catch (...)
            {
                if (fd >= 0) sys_close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        if (fd < 0) throw std::runtime_error("connect_tcp: failed to connect");
        return fd;
    }

    void send_all(int fd, const void* data, size_t len)
    {
        const char* p = static_cast<const char*>(data);
        while (len > 0)
        {
        #ifdef _WIN32
            int n = ::send(fd, p, (int)len, 0);
        #else
            ssize_t n = ::send(fd, p, len, 0);
        #endif
            if (n <= 0) throw std::system_error(errno, std::generic_category(), "send()");
            p += n; len -= (size_t)n;
        }
    }

    size_t recv_some(int fd, void* buf, size_t cap)
    {
        #ifdef _WIN32
        int n = ::recv(fd, static_cast<char*>(buf), (int)cap, 0);
        if (n > 0) return (size_t)n;
        if (n == 0) return 0; // peer closed
        int e = WSAGetLastError();
        // Non-blocking races: report “no data right now” instead of throwing
        if (e == WSAEWOULDBLOCK || e == WSAEINTR) return SIZE_MAX;
        throw std::system_error(e, std::system_category(), "recv()");
        #else
        ssize_t n;
        for (;;) {
            n = ::recv(fd, buf, cap, 0);
            if (n >= 0) break;
            if (errno == EINTR) continue; // retry interrupted syscalls
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; // no data now
            throw std::system_error(errno, std::generic_category(), "recv()");
        }
        return (size_t)n; // includes 0 = peer closed
        #endif
    }


    void close_fd(int fd)
    {
        sys_close(fd);
    }

    // ---------- non-blocking & readiness ----------

    void set_nonblocking(int fd, bool nb)
    {
        #ifdef _WIN32
        u_long on = nb ? 1UL : 0UL;
        if (::ioctlsocket(fd, FIONBIO, &on) != 0)
            throw std::system_error(WSAGetLastError(), std::system_category(), "ioctlsocket(FIONBIO)");
        #else
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        if (::fcntl(fd, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) != 0)
            throw std::system_error(errno, std::generic_category(), "fcntl(O_NONBLOCK)");
        #endif
    }

    bool wait_readable(int fd, int timeout_ms)
    {
        #ifdef _WIN32
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        timeval tv;
        timeval* ptv = nullptr;
        if (timeout_ms >= 0)
        {
            tv.tv_sec = timeout_ms/1000;
            tv.tv_usec = (timeout_ms%1000)*1000; ptv = &tv;
        }
        int rc = ::select(0, &rfds, nullptr, nullptr, ptv);
        if (rc < 0) throw std::system_error(WSAGetLastError(), std::system_category(), "select()");
        return rc > 0;
        #else
        struct pollfd pfd{fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc < 0) throw std::system_error(errno, std::generic_category(), "poll()");
        return rc > 0 && (pfd.revents & (POLLIN|POLLHUP));
        #endif
    }

    bool wait_writable(int fd, int timeout_ms)
    {
        #ifdef _WIN32
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        timeval tv; timeval* ptv = nullptr;
        if (timeout_ms >= 0)
        {
            tv.tv_sec = timeout_ms/1000;
            tv.tv_usec = (timeout_ms%1000)*1000; ptv = &tv;
        }
        int rc = ::select(0, nullptr, &wfds, nullptr, ptv);
        if (rc < 0) throw std::system_error(WSAGetLastError(), std::system_category(), "select()");
        return rc > 0;
        #else
        struct pollfd pfd{fd, POLLOUT, 0};
        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc < 0) throw std::system_error(errno, std::generic_category(), "poll()");
        return rc > 0 && (pfd.revents & POLLOUT);
        #endif
    }

    // ---------- scatter/gather wrappers ----------

    size_t recvv(int fd, IoVec* vecs, int count)
    {
        #ifdef _WIN32
        // Windows: WSARecv with WSABUF
        std::vector<WSABUF> w(count);
        DWORD flags = 0, bytes = 0;
        for (int i=0;i<count;++i)
        {
            w[i].buf = static_cast<char*>(vecs[i].base);
            w[i].len = (ULONG)vecs[i].len;
        }
        int rc = ::WSARecv(fd, w.data(), (DWORD)count, &bytes, &flags, nullptr, nullptr);
        if (rc == SOCKET_ERROR)
        {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 0;
            throw std::system_error(e, std::system_category(), "WSARecv");
        }
        return (size_t)bytes;
        #else
        std::vector<iovec> v(count);
        for (int i=0;i<count;++i){ v[i].iov_base = vecs[i].base; v[i].iov_len = vecs[i].len; }
        ssize_t n = ::readv(fd, v.data(), count);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            throw std::system_error(errno, std::generic_category(), "readv");
        }
        return (size_t)n;
        #endif
    }

    size_t sendv(int fd, const IoVec* vecs, int count)
    {
        #ifdef _WIN32
        std::vector<WSABUF> w(count);
        DWORD bytes = 0;
        for (int i=0;i<count;++i)
        {
            w[i].buf = (char*)vecs[i].base;
            w[i].len = (ULONG)vecs[i].len;
        }
        int rc = ::WSASend(fd, w.data(), (DWORD)count, &bytes, 0, nullptr, nullptr);
        if (rc == SOCKET_ERROR)
        {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 0;
            throw std::system_error(e, std::system_category(), "WSASend");
        }
        return (size_t)bytes;
        #else
        std::vector<iovec> v(count);
        for (int i=0;i<count;++i){ v[i].iov_base = const_cast<void*>(vecs[i].base); v[i].iov_len = vecs[i].len; }
        ssize_t n = ::writev(fd, v.data(), count);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            throw std::system_error(errno, std::generic_category(), "writev");
        }
        return (size_t)n;
        #endif
    }

    // ---------- Linux batch syscalls ----------

    int recvmmsg_batch(int fd, void** bufs, size_t* lens, int count)
    {
        #ifdef __linux__
        std::vector<mmsghdr> msgs(count);
        std::vector<iovec>   iov(count);
        for (int i=0;i<count;++i)
        {
            iov[i].iov_base = bufs[i];
            iov[i].iov_len  = lens[i];
            std::memset(&msgs[i], 0, sizeof(mmsghdr));
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }
        int rc = ::recvmmsg(fd, msgs.data(), count, MSG_DONTWAIT, nullptr);
        if (rc < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            throw std::system_error(errno, std::generic_category(), "recvmmsg");
        }
        // update lens with actual received sizes
        for (int i=0;i<rc;++i) lens[i] = msgs[i].msg_len;
        return rc;
        #else
        (void)fd;(void)bufs;(void)lens;(void)count;
        return 0;
        #endif
    }

    int sendmmsg_batch(int fd, const void** bufs, const size_t* lens, int count)
    {
        #ifdef __linux__
        std::vector<mmsghdr> msgs(count);
        std::vector<iovec>   iov(count);
        for (int i=0;i<count;++i)
        {
            iov[i].iov_base = const_cast<void*>(bufs[i]);
            iov[i].iov_len  = lens[i];
            std::memset(&msgs[i], 0, sizeof(mmsghdr));
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }
        int rc = ::sendmmsg(fd, msgs.data(), count, MSG_DONTWAIT);
        if (rc < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            throw std::system_error(errno, std::generic_category(), "sendmmsg");
        }
        return rc;
        #else
        (void)fd;(void)bufs;(void)lens;(void)count;
        return 0;
        #endif
    }

    void enable_zerocopy(int fd, bool on)
    {
        #ifdef __linux__
        int val = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val));
        #else
        (void)fd; (void)on;
        #endif
    }

} // namespace net
