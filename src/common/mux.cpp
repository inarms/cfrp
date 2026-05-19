/*
 * Copyright 2026 inarms
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
#include "common/utils.h"
#include <unordered_map>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <asio/post.hpp>

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
    : id_(id), session_(session), strand_(asio::make_strand(session->get_executor())), local_window_size_(256 * 1024), remote_window_size_(256 * 1024) {}

MuxStream::~MuxStream() {
    // Synchronous cleanup only in destructor
    local_closed_ = true;
    for (auto& pr : pending_reads_) {
        auto h = std::move(pr.handler);
        asio::post(strand_, [h]() { h(asio::error::operation_aborted, 0); });
    }
    pending_reads_.clear();
    
    for (auto& pw : pending_writes_) {
        auto h = std::move(pw.handler);
        asio::post(strand_, [h]() { h(asio::error::operation_aborted, 0); });
    }
    pending_writes_.clear();
}

// Bounds to defend against unbounded queue growth (peer not draining or local stall).
// If exceeded, the operation is failed with no_buffer_space rather than OOM-ing the process.
static constexpr size_t kMuxStreamMaxPendingReads = 1024;
static constexpr size_t kMuxStreamMaxPendingWrites = 1024;

void MuxStream::async_read_some(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), buffer, handler]() {
            async_read_some(buffer, handler);
        });
        return;
    }
    if (pending_reads_.size() >= kMuxStreamMaxPendingReads) {
        auto h = std::move(handler);
        asio::post(strand_, [h]() { h(asio::error::no_buffer_space, 0); });
        return;
    }
    pending_reads_.push_back({buffer, std::move(handler), false});
    do_read_from_buffer();
}

void MuxStream::async_write(asio::const_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), buffer, handler]() {
            async_write(buffer, handler);
        });
        return;
    }

    if (pending_writes_.size() >= kMuxStreamMaxPendingWrites) {
        auto h = std::move(handler);
        asio::post(strand_, [h]() { h(asio::error::no_buffer_space, 0); });
        return;
    }
    pending_writes_.push_back({{buffer}, std::move(handler)});
    do_write();
}

void MuxStream::async_write(const std::vector<asio::const_buffer>& buffers, std::function<void(std::error_code, std::size_t)> handler) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), buffers, handler]() {
            async_write(buffers, handler);
        });
        return;
    }

    if (pending_writes_.size() >= kMuxStreamMaxPendingWrites) {
        auto h = std::move(handler);
        asio::post(strand_, [h]() { h(asio::error::no_buffer_space, 0); });
        return;
    }
    pending_writes_.push_back({buffers, std::move(handler)});
    do_write();
}

void MuxStream::do_write() {
    while (!pending_writes_.empty() && remote_window_size_ > 0) {
        auto& pw = pending_writes_.front();
        size_t total_length = 0;
        for (const auto& b : pw.buffers) total_length += b.size();

        if (total_length == 0) {
            auto handler = std::move(pw.handler);
            pending_writes_.pop_front();
            asio::post(strand_, [handler]() { handler(std::error_code(), 0); });
            continue;
        }

        size_t to_send = std::min(total_length, remote_window_size_);
        
        auto session = session_.lock();
        if (!session) {
            auto handler = std::move(pw.handler);
            pending_writes_.pop_front();
            asio::post(strand_, [handler]() { handler(asio::error::connection_reset, 0); });
            continue;
        }

        Header h;
        h.version = 0;
        h.type = (uint8_t)Type::Data;
        h.flags = 0;
        h.stream_id = id_;
        h.length = static_cast<uint32_t>(to_send);

        // Slice the buffers to 'to_send'
        std::vector<asio::const_buffer> sliced_buffers;
        size_t remaining = to_send;
        for (const auto& b : pw.buffers) {
            if (remaining == 0) break;
            size_t take = std::min(remaining, b.size());
            sliced_buffers.push_back(asio::buffer(b.data(), take));
            remaining -= take;
        }

        remote_window_size_ -= to_send;
        
        if (to_send < total_length) {
            // Partial send: update pw.buffers for the next iteration
            std::vector<asio::const_buffer> next_buffers;
            size_t skipped = to_send;
            for (const auto& b : pw.buffers) {
                if (skipped >= b.size()) {
                    skipped -= b.size();
                } else {
                    next_buffers.push_back(asio::buffer((const uint8_t*)b.data() + skipped, b.size() - skipped));
                    skipped = 0;
                }
            }
            pw.buffers = std::move(next_buffers);
            
            session->async_send_frame(h, sliced_buffers); 
        } else {
            // Full send
            auto handler = std::move(pw.handler);
            pending_writes_.pop_front();
            session->async_send_frame(h, sliced_buffers, [this, self = shared_from_this(), handler, to_send](std::error_code ec) {
                if (!ec) handler(ec, to_send);
                else handler(ec, 0);
            });
        }
    }
}

void MuxStream::async_read(asio::mutable_buffer buffer, std::function<void(std::error_code, std::size_t)> handler) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), buffer, handler]() {
            async_read(buffer, handler);
        });
        return;
    }
    if (pending_reads_.size() >= kMuxStreamMaxPendingReads) {
        auto h = std::move(handler);
        asio::post(strand_, [h]() { h(asio::error::no_buffer_space, 0); });
        return;
    }
    pending_reads_.push_back({buffer, std::move(handler), true});
    do_read_from_buffer();
}

void MuxStream::async_handshake(asio::ssl::stream_base::handshake_type, std::function<void(std::error_code)> handler) {
    asio::post(get_executor(), [handler]() {
        handler(std::error_code());
    });
}

void MuxStream::close() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() {
            close();
        });
        return;
    }

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
        session->async_send_frame(h, asio::const_buffer{});
    }

    for (auto& pr : pending_reads_) {
        auto h = std::move(pr.handler);
        asio::post(strand_, [h]() {
            h(asio::error::operation_aborted, 0);
        });
    }
    pending_reads_.clear();
    
    for (auto& pw : pending_writes_) {
        auto h = std::move(pw.handler);
        asio::post(strand_, [h]() {
            h(asio::error::operation_aborted, 0);
        });
    }
    pending_writes_.clear();

    check_cleanup();
}

asio::any_io_executor MuxStream::get_executor() {
    return strand_;
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

void MuxStream::handle_data(std::shared_ptr<uint8_t[]> data, size_t length) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), data, length]() {
            handle_data(data, length);
        });
        return;
    }
    read_queue_.push_back({data, length});
    do_read_from_buffer();
}

void MuxStream::handle_window_update(uint32_t delta) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), delta]() {
            handle_window_update(delta);
        });
        return;
    }
    remote_window_size_ += delta;
    do_write();
}

void MuxStream::handle_close() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() {
            handle_close();
        });
        return;
    }
    remote_closed_ = true;
    do_read_from_buffer();
    check_cleanup();
}

void MuxStream::check_cleanup() {
    if (local_closed_ && remote_closed_ && read_queue_.empty()) {
        auto session = session_.lock();
        if (session) {
            session->remove_stream(id_);
        }
    }
}

void MuxStream::do_read_from_buffer() {
    while (!pending_reads_.empty()) {
        auto& pr = pending_reads_.front();
        
        if (read_queue_.empty()) {
            if (remote_closed_) {
                auto handler = std::move(pr.handler);
                pending_reads_.pop_front();
                asio::post(strand_, [handler]() {
                    handler(asio::error::eof, 0);
                });
                continue;
            }
            break;
        }

        size_t total_copied = 0;
        size_t buffer_remaining = pr.buffer.size();

        // Check if we can fulfill read_all
        if (pr.read_all) {
            size_t available = 0;
            for (const auto& block : read_queue_) available += block.length;
            available -= read_queue_offset_;
            
            if (available < buffer_remaining && !remote_closed_) {
                break; // Wait for more data
            }
        }

        while (buffer_remaining > 0 && !read_queue_.empty()) {
            auto& block = read_queue_.front();
            size_t vec_available = block.length - read_queue_offset_;
            size_t to_copy = std::min(buffer_remaining, vec_available);

            std::memcpy(static_cast<uint8_t*>(pr.buffer.data()) + total_copied,
                        block.data.get() + read_queue_offset_, to_copy);

            total_copied += to_copy;
            buffer_remaining -= to_copy;
            read_queue_offset_ += to_copy;

            if (read_queue_offset_ == block.length) {
                read_queue_.pop_front();
                read_queue_offset_ = 0;
            }
        }

        auto handler = std::move(pr.handler);
        pending_reads_.pop_front();
        
        asio::post(strand_, [handler, total_copied]() {
            handler(std::error_code(), total_copied);
        });

        // Send window update to peer
        consumed_since_last_update_ += total_copied;
        if (consumed_since_last_update_ >= 128 * 1024) {
            auto session = session_.lock();
            if (session) {
                Header h;
                h.version = 0;
                h.type = (uint8_t)Type::WindowUpdate;
                h.flags = 0;
                h.stream_id = id_;
                h.length = static_cast<uint32_t>(consumed_since_last_update_);
                session->async_send_frame(h, asio::const_buffer{});
                consumed_since_last_update_ = 0;
            }
        }
    }
    check_cleanup();
}

// --- Session ---

Session::Session(std::shared_ptr<AsyncStream> underlying_stream, bool is_server, std::shared_ptr<BufferPool> buffer_pool)
    : underlying_stream_(std::move(underlying_stream)), 
      strand_(asio::make_strand(underlying_stream_->get_executor())),
      buffer_pool_(std::move(buffer_pool)),
      is_server_(is_server), 
      next_stream_id_(is_server ? 2 : 1),
      heartbeat_timer_(strand_) {
    if (!buffer_pool_) buffer_pool_ = BufferPool::CreateDefault();
}

void Session::start(std::function<void(std::shared_ptr<MuxStream>)> on_new_stream) {
    on_new_stream_ = std::move(on_new_stream);
    asio::post(strand_, [this, self = shared_from_this()]() {
        do_read_header();
        schedule_heartbeat();
    });
}

void Session::stop() {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this()]() {
            stop();
        });
        return;
    }

    std::unordered_map<uint32_t, std::shared_ptr<MuxStream>> streams_to_close;
    {
        std::lock_guard<std::mutex> lock(mux_mutex_);
        if (stopped_) return;
        stopped_ = true;
        streams_to_close = std::move(streams_);
        streams_.clear();
    }

    // Clear the callback first to break any shared_ptr cycle where a caller
    // captures 'mux_session' (shared_ptr<Session>) inside the on_new_stream_ lambda.
    on_new_stream_ = nullptr;

    heartbeat_timer_.cancel();
    underlying_stream_->close();
    
    for (auto& [id, stream] : streams_to_close) {
        stream->handle_close();
    }
}

void Session::schedule_heartbeat() {
    heartbeat_timer_.expires_after(std::chrono::seconds(30));
    std::weak_ptr<Session> weak_self = shared_from_this();
    heartbeat_timer_.async_wait(asio::bind_executor(strand_, [this, weak_self](std::error_code ec) {
        if (!ec) {
            auto self = weak_self.lock();
            if (!self) return;
            Header h;
            h.version = 0;
            h.type = (uint8_t)Type::Ping;
            h.flags = Flags::SYN; // Ping request
            h.stream_id = 0;
            h.length = 0;
            async_send_frame(h, asio::const_buffer{});
            schedule_heartbeat();
        }
    }));
}

std::shared_ptr<MuxStream> Session::open_stream() {
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(mux_mutex_);
        id = next_stream_id_;
        next_stream_id_ += 2;
    }
    
    auto stream = std::make_shared<MuxStream>(id, shared_from_this());
    {
        std::lock_guard<std::mutex> lock(mux_mutex_);
        streams_[id] = stream;
    }
    
    Header h;
    h.version = 0;
    h.type = (uint8_t)Type::Data;
    h.flags = Flags::SYN;
    h.stream_id = id;
    h.length = 0;
    async_send_frame(h, asio::const_buffer{});
    
    return stream;
}

void Session::remove_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(mux_mutex_);
    if (streams_.erase(stream_id) > 0) {
        Logger::Debug("[MuxSession] Cleaned up and removed stream ID: " + std::to_string(stream_id));
    }
}

void Session::async_send_frame(Header h, const std::vector<asio::const_buffer>& bodies, std::function<void(std::error_code)> handler, std::shared_ptr<uint8_t[]> body_storage) {
    if (!strand_.running_in_this_thread()) {
        asio::post(strand_, [this, self = shared_from_this(), h, bodies, handler, body_storage]() mutable {
            async_send_frame(h, bodies, handler, body_storage);
        });
        return;
    }

    auto pw = std::make_shared<PendingWrite>();
    h.encode(pw->header_data);
    pw->body_storage = body_storage;
    pw->handler = std::move(handler);

    // Prepare the final write buffer sequence.
    // It must stay valid until the async_write operation completes.
    pw->write_buffers.reserve(1 + (bodies.empty() && body_storage ? 1 : bodies.size()));
    pw->write_buffers.push_back(asio::buffer(pw->header_data, Header::size));

    if (bodies.empty() && body_storage) {
        pw->write_buffers.push_back(asio::buffer(body_storage.get(), h.length));
    } else {
        for (const auto& b : bodies) {
            if (b.size() > 0) pw->write_buffers.push_back(b);
        }
    }

    write_queue_.push_back(pw);
    if (!is_writing_) {
        is_writing_ = true;
        do_write();
    }
}

void Session::async_send_frame(Header h, asio::const_buffer body, std::function<void(std::error_code)> handler, std::shared_ptr<uint8_t[]> body_storage) {
    std::vector<asio::const_buffer> bodies;
    if (body.size() > 0) bodies.push_back(body);
    async_send_frame(h, bodies, std::move(handler), std::move(body_storage));
}

void Session::do_read_header() {
    auto self = shared_from_this();
    underlying_stream_->async_read(asio::buffer(header_buf_, Header::size),
        asio::bind_executor(strand_, [this, self](std::error_code ec, std::size_t) {
            {
                std::lock_guard<std::mutex> lock(mux_mutex_);
                if (stopped_) return;
            }
            if (!ec) {
                auto header = Header::decode(header_buf_);
                if (header.type == (uint8_t)Type::Data && header.length > 0) {
                    do_read_body(header);
                } else {
                    handle_frame(header, {}, 0);
                    do_read_header();
                }
            } else {
                stop();
            }
        }));
}

void Session::do_read_body(Header h) {
    auto self = shared_from_this();
    auto body = buffer_pool_->Get(h.length);
    underlying_stream_->async_read(asio::buffer(body.get(), h.length),
        asio::bind_executor(strand_, [this, self, h, body](std::error_code ec, std::size_t) {
            {
                std::lock_guard<std::mutex> lock(mux_mutex_);
                if (stopped_) return;
            }
            if (!ec) {
                handle_frame(h, body, h.length);
                do_read_header();
            } else {
                stop();
            }
        }));
}

void Session::handle_frame(Header h, std::shared_ptr<uint8_t[]> body, size_t length) {
    std::shared_ptr<MuxStream> stream;
    {
        std::lock_guard<std::mutex> lock(mux_mutex_);
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
                    std::lock_guard<std::mutex> lock(mux_mutex_);
                    streams_[h.stream_id] = stream;
                }
                
                if (on_new_stream_) on_new_stream_(stream);
                
                Header resp;
                resp.version = 0;
                resp.type = (uint8_t)Type::Data;
                resp.flags = Flags::ACK;
                resp.stream_id = h.stream_id;
                resp.length = 0;
                async_send_frame(resp, asio::const_buffer{});
            }
        }
        
        if (stream) {
            if (length > 0) {
                stream->handle_data(body, length);
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
            async_send_frame(resp, asio::const_buffer{});
        }
    } else if (h.type == (uint8_t)Type::GoAway) {
        stop();
    } else {
        Logger::Error("[MuxSession] Unknown frame type: " + std::to_string(h.type) + " from " + remote_endpoint_string() + ". Closing session.");
        stop();
    }
}

void Session::do_write() {
    if (write_queue_.empty()) {
        is_writing_ = false;
        return;
    }
    auto pw = write_queue_.front();
    auto self = shared_from_this();
    
    // Use the pre-calculated write_buffers which is safe in pw
    underlying_stream_->async_write(pw->write_buffers,
        asio::bind_executor(strand_, [this, self, pw](std::error_code ec, std::size_t) {
            if (!write_queue_.empty() && write_queue_.front() == pw) {
                write_queue_.pop_front();
            }
            
            if (pw->handler) pw->handler(ec);
            
            if (!ec) {
                do_write();
            } else {
                stop();
            }
        }));
}

} // namespace mux
} // namespace common
} // namespace cfrp
