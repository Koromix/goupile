// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "../../core/libcc/libcc.hh"
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
#endif
#include "../../../vendor/libmicrohttpd/src/include/microhttpd.h"

namespace RG {

enum class http_ClientAddressMode {
    Socket,
    XForwardedFor,
    XRealIP
};
static const char *const http_ClientAddressModeNames[] = {
    "Socket",
    "X-Forwarded-For",
    "X-Real-IP"
};

struct http_Config {
    SocketType sock_type = SocketType::Dual;
    int port = 8888;
    const char *unix_path = nullptr;

    int max_connections = 2048;
    int idle_timeout = 60;
    int threads = std::max(GetCoreCount(), 4);
    int async_threads = std::max(GetCoreCount() * 4, 16);
    http_ClientAddressMode client_addr_mode = http_ClientAddressMode::Socket;

    bool Validate() const;
};

struct http_RequestInfo;
class http_IO;

class http_Daemon {
    RG_DELETE_COPY(http_Daemon)

    MHD_Daemon *daemon = nullptr;
    int listen_fd = -1;
    http_ClientAddressMode client_addr_mode = http_ClientAddressMode::Socket;

#ifdef _WIN32
    void *stop_handle = nullptr;
#else
    int stop_pfd[2] = {-1, -1};
#endif
    std::atomic_bool running {false};

    std::function<void(const http_RequestInfo &request, http_IO *io)> handle_func;

    Async *async = nullptr;

public:
    http_Daemon() {}
    ~http_Daemon() { Stop(); }

    bool Bind(const http_Config &config);
    bool Start(const http_Config &config,
               std::function<void(const http_RequestInfo &request, http_IO *io)> func);

    void Stop();

private:
    static MHD_Result HandleRequest(void *cls, MHD_Connection *conn, const char *url, const char *method,
                                    const char *, const char *upload_data, size_t *upload_data_size,
                                    void **con_cls);
    static ssize_t HandleWrite(void *cls, uint64_t pos, char *buf, size_t max);
    void RunNextAsync(http_IO *io);

    static void RequestCompleted(void *cls, MHD_Connection *, void **con_cls,
                                 MHD_RequestTerminationCode toe);

    friend http_IO;
};

enum class http_RequestMethod {
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Options
};
static const char *const http_RequestMethodNames[] = {
    "GET",
    "POST",
    "PUT",
    "PATCH",
    "DELETE",
    "OPTIONS"
};

struct http_RequestInfo {
    MHD_Connection *conn;

    // When verb is HEAD, method is set to Get and headers_only is set to true
    http_RequestMethod method;
    bool headers_only;
    const char *url;

    char client_addr[65];

    const char *GetHeaderValue(const char *key) const
        { return MHD_lookup_connection_value(conn, MHD_HEADER_KIND, key); }
    const char *GetQueryValue(const char *key) const
        { return MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, key); }
    const char *GetCookieValue(const char *key) const
        { return MHD_lookup_connection_value(conn, MHD_COOKIE_KIND, key); }
};

enum class http_WebSocketFlag {
    Text = 1 << 0
};

class http_IO {
    RG_DELETE_COPY(http_IO)

    enum class State {
        Sync,
        Idle,
        Async,
        WebSocket,
        Zombie
    };

    http_Daemon *daemon;
    http_RequestInfo request = {};

    int code = -1;
    MHD_Response *response = nullptr;

    std::mutex mutex;
    State state = State::Sync;
    bool suspended = false;

    std::function<void()> async_func;
    bool async_func_response = false;
    const char *last_err = nullptr;
    bool force_queue = false;

    std::condition_variable read_cv;
    Size read_max = -1;
    Span<uint8_t> read_buf = {};
    Size read_len = 0;
    Size read_total = 0;
    bool read_eof = false;

    int write_code;
    uint64_t write_len;
    std::condition_variable write_cv;
    HeapArray<uint8_t> write_buf;
    Size write_offset = 0;
    bool write_eof = false;

    int ws_opcode;
    std::condition_variable ws_cv;
    struct MHD_UpgradeResponseHandle *ws_urh = nullptr;
    MHD_socket ws_fd;
    HeapArray<uint8_t> ws_buf;
    Size ws_offset;
#ifdef _WIN32
    void *ws_handle;
#endif

    HeapArray<std::function<void()>> finalizers;

public:
    BlockAllocator allocator;

    http_IO();
    ~http_IO();

    bool NegociateEncoding(CompressionType preferred, CompressionType *out_encoding);
    bool NegociateEncoding(CompressionType preferred1, CompressionType preferred2, CompressionType *out_encoding);

    void RunAsync(std::function<void()> func);

    void AddHeader(const char *key, const char *value);
    void AddEncodingHeader(CompressionType encoding);
    void AddCookieHeader(const char *path, const char *name, const char *value,
                         bool http_only = false);
    void AddCachingHeaders(int max_age, const char *etag = nullptr);

    void AttachResponse(int code, MHD_Response *new_response);
    void AttachText(int code, Span<const char> str, const char *mime_type = "text/plain");
    bool AttachBinary(int code, Span<const uint8_t> data, const char *mime_type,
                      CompressionType compression_type = CompressionType::None);
    void AttachError(int code, const char *details = nullptr);
    bool AttachFile(int code, const char *filename);
    void AttachNothing(int code);

    void ResetResponse();

    bool IsWS() const;
    bool UpgradeWS(unsigned int flags, StreamReader *out_reader, StreamWriter *out_writer);

    // These must be run in async context (with RunAsync)
    bool OpenForRead(Size max_len, StreamReader *out_st);
    bool OpenForWrite(int code, Size len, CompressionType encoding, StreamWriter *out_st);
    bool OpenForWrite(int code, Size len, StreamWriter *out_st)
        { return OpenForWrite(code, len, CompressionType::None, out_st); }
    bool ReadPostValues(Allocator *alloc, HashMap<const char *, const char *> *out_values);

    void AddFinalizer(const std::function<void()> &func);

private:
    void PushLogFilter();

    Size Read(Span<uint8_t> out_buf);
    bool Write(Span<const uint8_t> buf);

    static void HandleUpgrade(void *cls, struct MHD_Connection *, void *,
                              const char *extra_in, size_t extra_in_size, MHD_socket fd,
                              struct MHD_UpgradeResponseHandle *urh);
    Size ReadWS(Span<uint8_t> out_buf);
    bool WriteWS(Span<const uint8_t> buf);

    // Call with mutex locked
    void Suspend();
    void Resume();

    friend http_Daemon;
};

}
