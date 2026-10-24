#ifndef YGO_MCP_STDIO_H
#define YGO_MCP_STDIO_H

#include <cstddef>
#include <string>

namespace ygo {

// Owns the JSON-RPC stdout descriptor. The GUI's stdout is redirected to
// stderr before initialization can print anything.
class MCPStdio {
public:
    bool Start();
    void Stop();
    // Positive: bytes read. Zero: no complete input yet. Negative: EOF/error.
    int Read(char* buffer, size_t capacity) const;
    bool Write(const std::string& line) const;

private:
    int output_fd_ = -1;
};

} // namespace ygo

#endif // YGO_MCP_STDIO_H
