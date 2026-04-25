/*
 * Copyright 2026 neesonqk
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
    // std::cout << "[MuxStream " << id_ << "] async_write: " << length << " bytes" << std::endl;
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
    if (local_closed_) return;
    local_closed_ = true;
    
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
    check_cleanup();
}

asio::any_io_executor MuxStream::get_executor() {
    auto session = session_.lock();
    if (session) return session->get_executor();
    return asio::system_executor();
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
}

void MuxStream::handle_window_update(uint32_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_window_size_ += delta;
}

void MuxStream::handle_close() {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_closed_ = true;
    do_read_from_buffer();
    check_cleanup();
}

void MuxStream::check_cleanup() {
    if (local_closed_ && remote_closed_ && read_buffer_.empty()) {
        auto session = session_.lock();
        if (session) {
            session->remove_stream(id_);
        }
    }
}

void MuxStream::do_read_from_buffer() {
    while (!pending_reads_.empty()) {
        auto& pr = pending_reads_.front();
        if (read_buffer_.empty()) {
            if (remote_closed_) {
                auto handler = std::move(pr.handler);
                pending_reads_.pop_front();
                asio::post(get_executor(), [handler]() {
                    handler(asio::error::eof, 0);
                });
                continue;
            }
            break;
        }

        size_t to_copy = std::min(pr.buffer.size(), read_buffer_.size());
        if (pr.read_all && to_copy < pr.buffer.size()) {
            if (remote_closed_) {
                auto handler = std::move(pr.handler);
                pending_reads_.pop_front();
                asio::post(get_executor(), [handler]() {
                    handler(asio::error::eof, 0);
                });
                continue;
            }
            break;
        }

        std::copy(read_buffer_.begin(), read_buffer_.begin() + to_copy, static_cast<uint8_t*>(pr.buffer.data()));
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + to_copy);
        
        auto handler = std::move(pr.handler);
        pending_reads_.pop_front();
        
        asio::post(get_executor(), [handler, to_copy]() {
            handler(std::error_code(), to_copy);
        });

        // Send window update to peer when a significant amount of data is consumed from our local buffer
        consumed_since_last_update_ += to_copy;
        if (consumed_since_last_update_ >= 128 * 1024) {
            auto session = session_.lock();
            if (session) {
                Header h;
                h.version = 0;
                h.type = (uint8_t)Type::WindowUpdate;
                h.flags = 0;
                h.stream_id = id_;
                h.length = static_cast<uint32_t>(consumed_since_last_update_);
                session->async_send_frame(h, {});
                consumed_since_last_update_ = 0;
            }
        }
    }
    check_cleanup();
}

// --- Session ---

Session::Session(std::shared_ptr<AsyncStream> underlying_stream, bool is_server)
    : underlying_stream_(std::move(underlying_stream)), 
      is_server_(is_server), 
      next_stream_id_(is_server ? 2 : 1),
      heartbeat_timer_(underlying_stream_->get_executor()) {}

void Session::start(std::function<void(std::shared_ptr<MuxStream>)> on_new_stream) {
    on_new_stream_ = std::move(on_new_stream);
    do_read_header();
    schedule_heartbeat();
}

void Session::stop() {
    heartbeat_timer_.cancel();
    underlying_stream_->close();
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [id, stream] : streams_) {
        stream->handle_close();
    }
    streams_.clear();
}

void Session::schedule_heartbeat() {
    heartbeat_timer_.expires_after(std::chrono::seconds(30));
    auto self = shared_from_this();
    heartbeat_timer_.async_wait([this, self](std::error_code ec) {
        if (!ec) {
            Header h;
            h.version = 0;
            h.type = (uint8_t)Type::Ping;
            h.flags = Flags::SYN; // Ping request
            h.stream_id = 0;
            h.length = 0;
            async_send_frame(h, {});
            schedule_heartbeat();
        }
    });
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

void Session::remove_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    if (streams_.erase(stream_id) > 0) {
        std::cout << "[MuxSession] Cleaned up and removed stream ID: " << stream_id << std::endl;
    }
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
    } else if (h.type == (uint8_t)Type::Ping) {
        if (h.flags & Flags::SYN) {
            Header resp;
            resp.version = 0;
            resp.type = (uint8_t)Type::Ping;
            resp.flags = Flags::ACK;
            resp.stream_id = 0;
            resp.length = 0;
            async_send_frame(resp, {});
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
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                if (!write_queue_.empty() && write_queue_.front() == pw) {
                    write_queue_.pop_front();
                }
            }
            
            if (pw->handler) pw->handler(ec);
            
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
