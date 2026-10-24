#include "mcp_stdio.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace ygo {

bool MCPStdio::Start() {
    if (output_fd_ >= 0)
        return true;
    std::fflush(stdout);
#ifdef _WIN32
    const int out = _fileno(stdout);
    const int err = _fileno(stderr);
    if (out < 0 || err < 0)
        return false;
    const int saved = _dup(out);
    if (saved < 0)
        return false;
    if (_setmode(saved, _O_BINARY) < 0 || _dup2(err, out) < 0) {
        _close(saved);
        return false;
    }
#else
    const int saved = ::dup(STDOUT_FILENO);
    if (saved < 0)
        return false;
    if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        ::close(saved);
        return false;
    }
#endif
    output_fd_ = saved;
    return true;
}

void MCPStdio::Stop() {
    if (output_fd_ < 0)
        return;
#ifdef _WIN32
    _close(output_fd_);
#else
    ::close(output_fd_);
#endif
    output_fd_ = -1;
}

int MCPStdio::Read(char* buffer, size_t capacity) const {
    if (!buffer || capacity == 0)
        return -1;
#ifdef _WIN32
    const int input = _fileno(stdin);
    if (input < 0)
        return -1;
    const intptr_t raw = _get_osfhandle(input);
    if (raw == -1)
        return -1;
    HANDLE handle = reinterpret_cast<HANDLE>(raw);
    const DWORD type = GetFileType(handle);
    if (type == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
            return -1;
        if (!available) {
            Sleep(25);
            return 0;
        }
        capacity = std::min(capacity, static_cast<size_t>(available));
    } else if (type == FILE_TYPE_CHAR) {
        const DWORD wait = WaitForSingleObject(handle, 25);
        if (wait == WAIT_TIMEOUT)
            return 0;
        if (wait != WAIT_OBJECT_0)
            return -1;
    } else if (type != FILE_TYPE_DISK) {
        return -1;
    }
    // Use the pipe's raw bytes. UCRT text-mode _read can wait for a byte after
    // a trailing CR even when PeekNamedPipe reports that the CR is available.
    DWORD count = 0;
    const DWORD requested = static_cast<DWORD>(std::min(capacity, static_cast<size_t>(INT_MAX)));
    if (!ReadFile(handle, buffer, requested, &count, nullptr))
        return -1;
    return count > 0 ? static_cast<int>(count) : -1;
#else
    pollfd fd{STDIN_FILENO, POLLIN | POLLHUP, 0};
    const int ready = ::poll(&fd, 1, 25);
    if (ready == 0 || (ready < 0 && errno == EINTR))
        return 0;
    if (ready < 0)
        return -1;
    const ssize_t count = ::read(STDIN_FILENO, buffer, capacity);
    if (count < 0 && errno == EINTR)
        return 0;
    return count > 0 ? static_cast<int>(count) : -1;
#endif
}

bool MCPStdio::Write(const std::string& line) const {
    if (output_fd_ < 0)
        return false;
    size_t offset = 0;
    while (offset < line.size()) {
        const size_t left = std::min(line.size() - offset, static_cast<size_t>(INT_MAX));
#ifdef _WIN32
        const int count = _write(output_fd_, line.data() + offset, static_cast<unsigned int>(left));
#else
        const ssize_t count = ::write(output_fd_, line.data() + offset, left);
        if (count < 0 && errno == EINTR)
            continue;
#endif
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

} // namespace ygo
