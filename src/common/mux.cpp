#include "common/mux.h"
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <asio/post.hpp>
#include <iostream>

namespace cfrp {
namespace common {
namespace mux {

// --- Header ---

void Header::encode(uint8_t* buf) const {
    buf[0] = version;
    buf[1] = type;
    uint16_t f = asio::detail::socket_ops::host_to_network_short(flags);
    std::memcpy(buf + 2, &f, 2);
    uint32_t s = asio::detail::socket_ops::host_to_network_long(stream_id);
    std::memcpy(buf + 4, &s, 4);
    uint32_t l = asio::detail::socket_ops::host_to_network_long(length);
    std::memcpy(buf + 8, &l, 4);
}

Header Header::decode(const uint8_t* buf) {
    Header h;
    h.version = buf[0];
    h.type = buf[1];
    uint16_t f;
    std::memcpy(&f, buf + 2, 2);
    h.flags = asio::detail::socket_ops::network_to_host_short(f);
    uint32_t s;
    std::memcpy(&s, buf + 4, 4);
    h.stream_id = asio::detail::socket_ops::network_to_host_long(s);
    uint32_t l;
    std::memcpy(&l, buf + 8, 4);
    h.length = asio::detail::socket_ops::network_to_host_long(l);
    return h;
}

// --- MuxStream ---

MuxStream::MuxStream(uint32_t id, std::shared_ptr<Session> session)
    : id_(id), session_(session), local_window_size_(256 * 1024), remote_window_size_(256 * 1024) {}

MuxStream::~MuxStream() {
    close();
}

void MuxStream::async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reads_.push_back({buffer, std::move(handler), false});
    do_read_from_buffer();
}

void MuxStream::async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    auto self(shared_from_this());
    auto session = session_.lock();
    if (!session) {
        asio::post(get_executor(), [handler]() {
            handler(asio::error::connection_reset, 0);
        });
        return;
    }

    size_t length = buffer.size();
    std::vector<uint8_t> body(static_cast<const uint8_t*>(buffer.data()), static_cast<const uint8_t*>(buffer.data()) + length);
    
    Header h;
    h.version = 0;
    h.type = (uint8_t)Type::Data;
    h.flags = 0;
    h.stream_id = id_;
    h.length = static_cast<uint32_t>(length);
    
    session->async_send_frame(h, std::move(body), [self, handler, length](std::error_code ec) {
        if (!ec) {
            handler(ec, length);
        } else {
            handler(ec, 0);
        }
    });
}

void MuxStream::async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reads_.push_back({buffer, std::move(handler), true});
    do_read_from_buffer();
}

void MuxStream::async_handshake(asio::ssl::stream_base::handshake_type, std::function<void(std::error_code)> handler) {
    asio::post(get_executor(), [handler]() {
        handler(std::error_code());
    });
}

void MuxStream::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true;
    
    auto session = session_.lock();
    if (session) {
        Header h;
        h.version = 0;
        h.type = (uint8_t)Type::Data;
        h.flags = Flags::FIN;
        h.stream_id = id_;
        h.length = 0;
        session->async_send_frame(h, {});
    }

    for (auto& pr : pending_reads_) {
        asio::post(get_executor(), [handler = std::move(pr.handler)]() {
            handler(asio::error::operation_aborted, 0);
        });
    }
    pending_reads_.clear();
}

asio::any_io_executor MuxStream::get_executor() {
    auto session = session_.lock();
    if (session) return session->get_executor();
    throw asio::system_error(asio::error::operation_aborted);
}

std::string MuxStream::remote_endpoint_string() {
    auto session = session_.lock();
    if (session) return session->remote_endpoint_string();
    return "unknown";
}

std::string MuxStream::protocol_name() {
    auto session = session_.lock();
    if (session) return session->protocol_name();
    return "unknown";
}

void MuxStream::handle_data(std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());
    do_read_from_buffer();
    
    auto session = session_.lock();
    if (session && read_buffer_.size() < 128 * 1024) {
        Header h;
        h.version = 0;
        h.type = (uint8_t)Type::WindowUpdate;
        h.flags = 0;
        h.stream_id = id_;
        h.length = static_cast<uint32_t>(data.size());
        session->async_send_frame(h, {});
    }
}

void MuxStream::handle_window_update(uint32_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_window_size_ += delta;
}

void MuxStream::handle_close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    for (auto& pr : pending_reads_) {
        if (read_buffer_.empty()) {
            asio::post(get_executor(), [handler = std::move(pr.handler)]() {
                handler(asio::error::eof, 0);
            });
        }
    }
    if (read_buffer_.empty()) pending_reads_.clear();
}

void MuxStream::do_read_from_buffer() {
    while (!pending_reads_.empty() && !read_buffer_.empty()) {
        auto& pr = pending_reads_.front();
        size_t to_copy = std::min(pr.buffer.size(), read_buffer_.size());
        
        if (pr.read_all && to_copy < pr.buffer.size()) {
            break;
        }

        std::copy(read_buffer_.begin(), read_buffer_.begin() + to_copy, static_cast<uint8_t*>(pr.buffer.data()));
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + to_copy);
        
        auto handler = std::move(pr.handler);
        pending_reads_.pop_front();
        
        asio::post(get_executor(), [handler, to_copy]() {
            handler(std::error_code(), to_copy);
        });
    }
}

// --- Session ---

Session::Session(std::shared_ptr<AsyncStream> underlying_stream, bool is_server)
    : underlying_stream_(std::move(underlying_stream)), is_server_(is_server), next_stream_id_(is_server ? 2 : 1) {}

void Session::start(std::function<void(std::shared_ptr<MuxStream>)> on_new_stream) {
    on_new_stream_ = std::move(on_new_stream);
    do_read_header();
}

void Session::stop() {
    underlying_stream_->close();
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [id, stream] : streams_) {
        stream->handle_close();
    }
    streams_.clear();
}

std::shared_ptr<MuxStream> Session::open_stream() {
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        id = next_stream_id_;
        next_stream_id_ += 2;
    }
    
    auto stream = std::make_shared<MuxStream>(id, shared_from_this());
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        streams_[id] = stream;
    }
    
    Header h;
    h.version = 0;
    h.type = (uint8_t)Type::Data;
    h.flags = Flags::SYN;
    h.stream_id = id;
    h.length = 0;
    async_send_frame(h, {});
    
    return stream;
}

void Session::async_send_frame(Header h, std::vector<uint8_t> body, std::function<void(std::error_code)> handler) {
    auto pw = std::make_shared<PendingWrite>();
    pw->data.resize(Header::size + body.size());
    h.encode(pw->data.data());
    if (!body.empty()) {
        std::memcpy(pw->data.data() + Header::size, body.data(), body.size());
    }
    pw->handler = std::move(handler);

    bool kick_write = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.push_back(pw);
        if (!is_writing_) {
            is_writing_ = true;
            kick_write = true;
        }
    }
    
    if (kick_write) {
        do_write();
    }
}

void Session::do_read_header() {
    auto self = shared_from_this();
    underlying_stream_->async_read(asio::buffer(header_buf_, Header::size),
        [this, self](std::error_code ec, std::size_t) {
            if (!ec) {
                auto header = Header::decode(header_buf_);
                if (header.type == (uint8_t)Type::Data && header.length > 0) {
                    do_read_body(header);
                } else {
                    handle_frame(header, {});
                    do_read_header();
                }
            } else {
                stop();
            }
        });
}

void Session::do_read_body(Header h) {
    auto self = shared_from_this();
    auto body = std::make_shared<std::vector<uint8_t>>(h.length);
    underlying_stream_->async_read(asio::buffer(*body),
        [this, self, h, body](std::error_code ec, std::size_t) {
            if (!ec) {
                handle_frame(h, std::move(*body));
                do_read_header();
            } else {
                stop();
            }
        });
}

void Session::handle_frame(Header h, std::vector<uint8_t> body) {
    std::shared_ptr<MuxStream> stream;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(h.stream_id);
        if (it != streams_.end()) {
            stream = it->second;
        }
    }

    if (h.type == (uint8_t)Type::Data) {
        if (h.flags & Flags::SYN) {
            if (!stream) {
                stream = std::make_shared<MuxStream>(h.stream_id, shared_from_this());
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    streams_[h.stream_id] = stream;
                }
                
                if (on_new_stream_) on_new_stream_(stream);
                
                Header resp;
                resp.version = 0;
                resp.type = (uint8_t)Type::Data;
                resp.flags = Flags::ACK;
                resp.stream_id = h.stream_id;
                resp.length = 0;
                async_send_frame(resp, {});
            }
        }
        
        if (stream) {
            if (!body.empty()) {
                stream->handle_data(std::move(body));
            }
            if (h.flags & Flags::FIN) {
                stream->handle_close();
            }
        }
    } else if (h.type == (uint8_t)Type::WindowUpdate) {
        if (stream) {
            stream->handle_window_update(h.length);
        }
    } else if (h.type == (uint8_t)Type::GoAway) {
        stop();
    }
}

void Session::do_write() {
    std::shared_ptr<PendingWrite> pw;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            is_writing_ = false;
            return;
        }
        pw = write_queue_.front();
    }

    auto self = shared_from_this();
    underlying_stream_->async_write(asio::buffer(pw->data),
        [this, self, pw](std::error_code ec, std::size_t) {
            if (pw->handler) pw->handler(ec);
            
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                write_queue_.pop_front();
            }
            
            if (!ec) {
                do_write();
            } else {
                stop();
            }
        });
}

} // namespace mux
} // namespace common
} // namespace cfrp
