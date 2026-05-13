module;
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

export module conflux.net.http.realtime;

import std;
import conflux.types;

export enum class SseOverflowPolicy : u8 {
	DropNewest,
	DropOldest,
	Disconnect,
};
export class SseChannel {
private:
	int efd_{};
	mutex mtx_{};
	std::queue<S> pending_{};
	atomic_flag closed_{};
	SZ queued_bytes_{0};
	SZ max_queue_bytes_{};
	SseOverflowPolicy overflow_{SseOverflowPolicy::DropNewest};
	Atom<SZ> dropped_{0};

public:
	static constexpr SZ kDefaultMaxQueueBytes = SZ{4} * 1024 * 1024;

	SseChannel(SseChannel const &) = delete;
	SseChannel &operator =(SseChannel const &) = delete;
	SseChannel(SseChannel &&) = delete;
	SseChannel &operator =(SseChannel &&) = delete;
	explicit SseChannel(
		SZ max_queue_bytes = kDefaultMaxQueueBytes,
		SseOverflowPolicy overflow = SseOverflowPolicy::DropNewest)
		: efd_(::eventfd(0, EFD_CLOEXEC))
		, max_queue_bytes_(max_queue_bytes)
		, overflow_(overflow) {
		if (efd_ < 0) {
			throw SE{errno, system_category(), "eventfd"};
		}
	}
	~SseChannel() noexcept {
		try {
			close();
		} catch (...) {} // NOLINT(bugprone-empty-catch): dtor must not propagate
		::close(efd_);
	}
	// Returns true if the frame was enqueued, false if dropped (overflow).
	// Also returns false if the channel is closed, regardless of policy.
	// Channel takes ownership of frame.
	[[nodiscard]] bool send(
		S frame) {
		bool enqueued = false;
		bool wake = false;
		{
			SL const lk{mtx_};
			if (closed_.test()) {
				return false;
			}
			SZ const frame_bytes = frame.size();
			SZ const would_be = queued_bytes_ + frame_bytes;
			if (would_be > max_queue_bytes_ && max_queue_bytes_ != 0) {
				switch (overflow_) {
				case SseOverflowPolicy::DropNewest: dropped_.fetch_add(1, memory_order_relaxed); return false;
				case SseOverflowPolicy::DropOldest:
					while (!pending_.empty() && queued_bytes_ + frame_bytes > max_queue_bytes_) {
						queued_bytes_ -= pending_.front().size();
						pending_.pop();
						dropped_.fetch_add(1, memory_order_relaxed);
					}
					break;
				case SseOverflowPolicy::Disconnect:
					closed_.test_and_set();
					dropped_.fetch_add(1, memory_order_relaxed);
					wake = true;
					break;
				}
			}
			if (!closed_.test()) {
				queued_bytes_ += frame_bytes;
				pending_.push(move(frame));
				enqueued = true;
				wake = true;
			}
		}
		if (wake) {
			u64 v = 1;
			if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
				eprintln(format("SseChannel::send: eventfd write: {}", strerror(errno)));
			}
		}
		return enqueued;
	}
	// Zero-copy intent: caller owns the backing buffer and is responsible for
	// keeping it alive until the frame is flushed to the socket. Currently
	// copies into the queue; when the queue migrates to SV storage this
	// contract becomes a hard lifetime requirement.
	[[nodiscard]] bool send_view(
		SV frame) {
		return send(S{frame});
	}
	[[nodiscard]] bool send_event(
		SV type,
		SV data) {
		// Reject newlines in type and data: they would break SSE framing and
		// allow injection of arbitrary events.
		auto has_nl = [](SV s) { return s.find('\n') != SV::npos || s.find('\r') != SV::npos; };
		if (has_nl(type) || has_nl(data)) {
			throw std::invalid_argument{"SseChannel::send_event: type and data must not contain newlines"};
		}
		return send(format("event: {}\ndata: {}\n\n", type, data));
	}
	void close() {
		if (closed_.test_and_set()) {
			return;
		} // already closed
		u64 v = 1;
		if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
			eprintln(format("SseChannel::close: eventfd write: {}", strerror(errno)));
		} // wake the io_uring poll
	}
	[[nodiscard]] S drain() {
		SL const lk{mtx_};
		S result;
		while (!pending_.empty()) {
			result += pending_.front();
			pending_.pop();
		}
		queued_bytes_ = 0;
		return result;
	}
	[[nodiscard]] bool is_closed() const noexcept { return closed_.test(); }
	[[nodiscard]] int eventfd_fd() const noexcept { return efd_; }
	[[nodiscard]] SZ dropped_count() const noexcept { return dropped_.load(memory_order_relaxed); }
	[[nodiscard]] SZ max_queue_bytes() const noexcept { return max_queue_bytes_; }
};

// ---------------------------------------------------------------------------
// SseBroadcaster: fan-out pub/sub for SSE streams.
// ---------------------------------------------------------------------------
// Maintains a set of active SseChannel weak_ptrs.  broadcast() delivers an
// SSE event to every currently-connected subscriber.  Stale weak_ptrs are
// reaped on each broadcast call.
export class SseBroadcaster {
public:
	SseBroadcaster() = default;
	~SseBroadcaster() = default;
	SseBroadcaster(SseBroadcaster const &) = delete;
	SseBroadcaster &operator =(SseBroadcaster const &) = delete;
	SseBroadcaster(SseBroadcaster &&) = delete;
	SseBroadcaster &operator =(SseBroadcaster &&) = delete;
	// Register a new subscriber.  Returns the SP to pass to HttpResponse::sse().
	SP<SseChannel> subscribe() {
		auto ch = make_shared<SseChannel>();
		SL const lk{mtx_};
		channels_.emplace_back(ch);
		return ch;
	}
	// Broadcast an SSE event to all active subscribers.
	void broadcast(
		SV event,
		SV data) {
		auto frame = format("event: {}\ndata: {}\n\n", event, data);
		broadcast_raw(frame);
	}
	// Broadcast a data-only SSE message to all active subscribers.
	void broadcast_data(
		SV data) {
		auto frame = format("data: {}\n\n", data);
		broadcast_raw(frame);
	}
	// Number of currently-active subscribers (approximate; may include ones
	// that have just disconnected).
	[[nodiscard]] SZ subscriber_count() const {
		SL const lk{mtx_};
		return channels_.size();
	}

private:
	void broadcast_raw(
		S const &frame) {
		SL const lk{mtx_};
		// Erase stale weak_ptrs while delivering to live ones.
		std::erase_if(channels_, [&](weak_ptr<SseChannel> const &wch) {
			auto ch = wch.lock();
			if (!ch || ch->is_closed()) {
				return true;
			}
			auto _ = ch->send(frame);
			return false;
		});
	}
	mutable mutex mtx_;
	V<weak_ptr<SseChannel>> channels_;
};
