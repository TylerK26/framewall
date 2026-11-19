// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "control.h"

#include "log.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Connects with a timeout; -1 if nobody is listening.
int connect_to(const std::string &host, int port, int timeout_ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            rc = poll(&p, 1, timeout_ms) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0 ? 0 : -1;
        }
        if (rc != 0) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

}  // namespace

ControlLink::ControlLink(std::string host, int port) : host_(std::move(host)), port_(port), thread_([this] { run(); }) {}

ControlLink::~ControlLink() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    }
    thread_.join();
}

void ControlLink::poll(bool &connected_now, std::map<std::string, std::string> &sets) {
    std::lock_guard<std::mutex> l(mu_);
    connected_now = fresh_;
    fresh_ = false;
    sets.swap(pending_);
    pending_.clear();
}

void ControlLink::send(const std::string &text) {
    std::lock_guard<std::mutex> l(mu_);
    if (fd_ < 0) return;
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::send(fd_, text.data() + off, text.size() - off, MSG_NOSIGNAL);
        if (n <= 0) { shutdown(fd_, SHUT_RDWR); return; }  // the reader notices and reconnects
        off += size_t(n);
    }
}

void ControlLink::run() {
    auto nap = [this](int ms) {
        for (int t = 0; t < ms && !stop_; t += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    while (!stop_) {
        const int fd = connect_to(host_, port_, 1000);
        if (fd < 0) { nap(2000); continue; }  // editor not open: try again quietly
        {
            std::lock_guard<std::mutex> l(mu_);
            fd_ = fd;
            fresh_ = true;
        }
        fwlog("panel editor: connected (%s:%d)", host_.c_str(), port_);
        std::string buf;
        char chunk[4096];
        while (!stop_) {
            pollfd p{fd, POLLIN, 0};
            const int r = ::poll(&p, 1, 200);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
            const ssize_t n = recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) break;
            buf.append(chunk, size_t(n));
            if (buf.size() > 65536) break;  // not the editor
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // set KEY VALUE
                if (line.compare(0, 4, "set ") != 0) continue;
                const auto sp = line.find(' ', 4);
                if (sp == std::string::npos) continue;
                std::lock_guard<std::mutex> l(mu_);
                pending_[line.substr(4, sp - 4)] = line.substr(sp + 1);
            }
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            fd_ = -1;
        }
        close(fd);
        if (!stop_) fwlog("panel editor: disconnected");
        nap(1000);
    }
}
