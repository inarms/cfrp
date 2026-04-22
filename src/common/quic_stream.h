#pragma once
#include <atomic>

#include "common/stream.h"
#include <msquic.h>
#include <asio.hpp>
#include <queue>
#include <mutex>
#include <vector>

namespace cfrp {
namespace common {

class QuicStream : public AsyncStream {
public:
    QuicStream(asio::any_io_executor executor, HQUIC connection_handle, HQUIC stream_handle);
    ~QuicStream() override;

    void async_read_some(asio::mutable_buffer buffer, 
                         std::function<void(std::error_code, std::size_t)> handler) override;
                                 
    void async_write(asio::const_buffer buffer, 
                     std::function<void(std::error_code, std::size_t)> handler) override;

    void async_read(asio::mutable_buffer buffer,
                    std::function<void(std::error_code, std::size_t)> handler) override;

    void async_handshake(ssl::stream_base::handshake_type type,
                         std::function<void(std::error_code)> handler) override;

    void close() override;
    void shutdown_transport() override;
    asio::any_io_executor get_executor() override;
    std::string remote_endpoint_string() override;
    std::string protocol_name() override;

    // Static MsQuic helper functions
    static const QUIC_API_TABLE* MsQuic;
    static HQUIC Registration;
    static HQUIC Configuration;
    static bool InitializeMsQuic(bool is_server, const std::string& cert_file = "",
                              const std::string& key_file = "", bool verify_peer = true);
    static void DeinitializeMsQuic();
    static void TrackConnection(HQUIC connection);
    static void UntrackConnection(HQUIC connection);
    static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);

private:
    static std::vector<HQUIC> ActiveConnections;
    static std::mutex ConnectionsMutex;

    asio::any_io_executor executor_;
    HQUIC connection_handle_;
    HQUIC stream_handle_;
    
    struct ReadOperation {
        asio::mutable_buffer buffer;
        std::function<void(std::error_code, std::size_t)> handler;
        bool read_some;
    };
    
    struct WriteContext {
        std::function<void(std::error_code, std::size_t)> handler;
        size_t size;
        QUIC_BUFFER quic_buffer;
    };

    struct WriteOperation {
        asio::const_buffer buffer;
        std::function<void(std::error_code, std::size_t)> handler;
    };

    std::mutex mutex_;
    std::queue<ReadOperation> pending_reads_;
    std::queue<WriteOperation> pending_writes_;
    
    std::vector<uint8_t> receive_buffer_;
    size_t receive_buffer_offset_ = 0;
    bool connected_ = false;
    std::atomic<bool> closed_ = false;
    
    void process_reads();
    void process_writes();
};

} // namespace common
} // namespace cfrp
