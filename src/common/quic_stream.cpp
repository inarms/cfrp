#include "common/quic_stream.h"
#include <iostream>

namespace cfrp {
namespace common {

const QUIC_API_TABLE* QuicStream::MsQuic = nullptr;
HQUIC QuicStream::Registration = nullptr;
HQUIC QuicStream::Configuration = nullptr;
std::vector<HQUIC> QuicStream::ActiveConnections;
std::mutex QuicStream::ConnectionsMutex;

bool QuicStream::InitializeMsQuic(bool is_server, const std::string& cert_file, const std::string& key_file) {
    if (MsQuic) return true;

    QUIC_STATUS status = MsQuicOpen2(&MsQuic);
    if (QUIC_FAILED(status)) {
        std::cerr << "MsQuicOpen2 failed: " << status << std::endl;
        return false;
    }

    const QUIC_REGISTRATION_CONFIG reg_config = { "cfrp", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    status = MsQuic->RegistrationOpen(&reg_config, &Registration);
    if (QUIC_FAILED(status)) {
        std::cerr << "RegistrationOpen failed: " << status << std::endl;
        return false;
    }

    QUIC_SETTINGS settings{0};
    settings.IdleTimeoutMs = 30000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.KeepAliveIntervalMs = 20000;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    settings.PeerUnidiStreamCount = 100;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.PeerBidiStreamCount = 100;
    settings.IsSet.PeerBidiStreamCount = TRUE;

    QUIC_BUFFER alpn;
    alpn.Buffer = (uint8_t*)"cfrp";
    alpn.Length = 4;

    if (is_server) {
        if (cert_file.empty() || key_file.empty()) {
            std::cerr << "QUIC Server requires cert_file and key_file" << std::endl;
            return false;
        }
        QUIC_CERTIFICATE_FILE cert_file_info = { key_file.c_str(), cert_file.c_str() };
        QUIC_CREDENTIAL_CONFIG cred_config;
        memset(&cred_config, 0, sizeof(cred_config));
        cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        cred_config.CertificateFile = &cert_file_info;
        cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE; // Lack of client flag indicates server.

        status = MsQuic->ConfigurationOpen(Registration, &alpn, 1, &settings, sizeof(settings), nullptr, &Configuration);
        if (QUIC_SUCCEEDED(status)) {
            status = MsQuic->ConfigurationLoadCredential(Configuration, &cred_config);
        }
    } else {
        QUIC_CREDENTIAL_CONFIG cred_config;
        memset(&cred_config, 0, sizeof(cred_config));
        cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

        status = MsQuic->ConfigurationOpen(Registration, &alpn, 1, &settings, sizeof(settings), nullptr, &Configuration);
        if (QUIC_SUCCEEDED(status)) {
            status = MsQuic->ConfigurationLoadCredential(Configuration, &cred_config);
        }
    }

    if (QUIC_FAILED(status)) {
        std::cerr << "Configuration failed: " << status << std::endl;
        return false;
    }

    return true;
}

void QuicStream::DeinitializeMsQuic() {
    if (MsQuic) {
        std::cout << "Deinitializing MsQuic..." << std::endl;
        
        {
            std::lock_guard<std::mutex> lock(ConnectionsMutex);
            for (auto conn : ActiveConnections) {
                MsQuic->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
            }
        }

        if (Configuration) MsQuic->ConfigurationClose(Configuration);
        if (Registration) MsQuic->RegistrationClose(Registration);
        MsQuicClose(MsQuic);
        MsQuic = nullptr;
    }
}

void QuicStream::TrackConnection(HQUIC connection) {
    std::lock_guard<std::mutex> lock(ConnectionsMutex);
    ActiveConnections.push_back(connection);
}

void QuicStream::UntrackConnection(HQUIC connection) {
    std::lock_guard<std::mutex> lock(ConnectionsMutex);
    auto it = std::find(ActiveConnections.begin(), ActiveConnections.end(), connection);
    if (it != ActiveConnections.end()) {
        ActiveConnections.erase(it);
    }
}

QuicStream::QuicStream(asio::any_io_executor executor, HQUIC stream_handle)
    : executor_(executor), stream_handle_(stream_handle) {
    MsQuic->SetCallbackHandler(stream_handle_, (void*)StreamCallback, this);
    connected_ = true;
}

QuicStream::~QuicStream() {
    close();
}

void QuicStream::async_read_some(asio::mutable_buffer buffer, 
                                 std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reads_.push({buffer, std::move(handler), true});
    process_reads();
}

void QuicStream::async_read(asio::mutable_buffer buffer,
                            std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reads_.push({buffer, std::move(handler), false});
    process_reads();
}

void QuicStream::async_write(asio::const_buffer buffer, 
                             std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_writes_.push({buffer, std::move(handler)});
    process_writes();
}

void QuicStream::async_handshake(ssl::stream_base::handshake_type type,
                                 std::function<void(std::error_code)> handler) {
    asio::post(executor_, [handler]() {
        handler(std::error_code());
    });
}

void QuicStream::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closed_) {
        closed_ = true;
        if (stream_handle_) {
            MsQuic->StreamClose(stream_handle_);
            stream_handle_ = nullptr;
        }
        process_reads();
    }
}

asio::any_io_executor QuicStream::get_executor() {
    return executor_;
}

std::string QuicStream::remote_endpoint_string() {
    return "quic-endpoint";
}

std::string QuicStream::protocol_name() {
    return "QUIC";
}

void QuicStream::process_reads() {
    while (!pending_reads_.empty() && (!receive_buffer_.empty() || closed_)) {
        auto& op = pending_reads_.front();
        size_t available = receive_buffer_.size() - receive_buffer_offset_;
        
        if (available > 0) {
            size_t to_copy = std::min(available, op.buffer.size());
            std::memcpy(op.buffer.data(), receive_buffer_.data() + receive_buffer_offset_, to_copy);
            receive_buffer_offset_ += to_copy;
            
            auto handler = std::move(op.handler);
            pending_reads_.pop();
            asio::post(executor_, [handler, transferred = to_copy]() {
                handler(std::error_code(), transferred);
            });
        } else if (closed_) {
            auto handler = std::move(op.handler);
            pending_reads_.pop();
            asio::post(executor_, [handler]() {
                handler(asio::error::eof, 0);
            });
        }
        
        if (receive_buffer_offset_ > 0 && receive_buffer_offset_ == receive_buffer_.size()) {
            receive_buffer_.clear();
            receive_buffer_offset_ = 0;
        }
    }
}

void QuicStream::process_writes() {
    while (!pending_writes_.empty()) {
        auto& op = pending_writes_.front();
        if (closed_) {
            auto handler = std::move(op.handler);
            pending_writes_.pop();
            asio::post(executor_, [handler]() {
                handler(asio::error::operation_aborted, 0);
            });
            continue;
        }

        WriteContext* context = new WriteContext{std::move(op.handler), op.buffer.size(), {}};
        context->quic_buffer.Length = (uint32_t)op.buffer.size();
        context->quic_buffer.Buffer = (uint8_t*)op.buffer.data();

        QUIC_STATUS status = MsQuic->StreamSend(stream_handle_, &context->quic_buffer, 1, QUIC_SEND_FLAG_NONE, context);
        
        pending_writes_.pop();
        
        if (QUIC_FAILED(status)) {
            std::cerr << "MsQuic StreamSend failed: " << status << std::endl;
            auto handler = std::move(context->handler);
            delete context;
            asio::post(executor_, [handler]() {
                handler(std::make_error_code(std::errc::io_error), 0);
            });
        }
    }
}

QUIC_STATUS QUIC_API QuicStream::StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    auto self = (QuicStream*)Context;
    switch (Event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            std::lock_guard<std::mutex> lock(self->mutex_);
            size_t total_length = 0;
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER* buffer = &Event->RECEIVE.Buffers[i];
                self->receive_buffer_.insert(self->receive_buffer_.end(), buffer->Buffer, buffer->Buffer + buffer->Length);
                total_length += buffer->Length;
            }
            Event->RECEIVE.TotalBufferLength = total_length;
            self->process_reads();
            return QUIC_STATUS_SUCCESS;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            WriteContext* context = (WriteContext*)Event->SEND_COMPLETE.ClientContext;
            if (context) {
                auto handler = std::move(context->handler);
                size_t size = context->size;
                asio::post(self->executor_, [handler, size]() {
                    handler(std::error_code(), size);
                });
                delete context;
            }
            return QUIC_STATUS_SUCCESS;
        }
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            self->close();
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->closed_ = true;
                self->process_reads();
            }
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->closed_ = true;
                self->process_reads();
            }
            return QUIC_STATUS_SUCCESS;
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

} // namespace common
} // namespace cfrp
