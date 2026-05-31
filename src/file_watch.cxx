module;

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

export module conflux.file_watch;

import std;
import conflux.types;
import conflux.file_io;
import conflux.uring.completion;

namespace conflux::file_watch {

export enum class FileEventKind : std::uint8_t {
	created,
	modified,
	removed,
	moved_from,
	moved_to,
	overflow,
};
export struct FileEvent {
	FileEventKind kind{};
	std::string path{};
	std::uint32_t cookie{};
	bool is_directory{};
};
export struct WatchOptions {
	std::uint32_t mask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
};
export class FileWatcher {
	struct Impl {
		int fd = -1;
		std::unordered_map<int, std::string> watches{};
		std::function<void(std::vector<FileEvent>)> on_events{};
		std::function<void(std::exception_ptr)> on_error{};
		std::atomic_flag started{};
		std::atomic_flag stopped{};
		std::mutex callback_mtx{};
		std::mutex watches_mtx{};
		~Impl() {
			if (fd >= 0) {
				::close(fd);
				fd = -1;
			}
		}
		[[nodiscard]] std::string root_for(
			int wd) {
			std::scoped_lock const lk{watches_mtx};
			auto it = watches.find(wd);
			return it != watches.end() ? it->second : std::string{};
		}
		void emit_error(
			std::exception_ptr eptr) {
			std::function<void(std::exception_ptr)> cb;
			{
				std::scoped_lock const lk{callback_mtx};
				cb = on_error;
			}
			if (cb) {
				cb(eptr);
			}
		}
		void emit(
			std::vector<FileEvent> events) {
			if (events.empty()) {
				return;
			}
			std::function<void(std::vector<FileEvent>)> cb;
			{
				std::scoped_lock const lk{callback_mtx};
				cb = on_events;
			}
			if (cb) {
				cb(std::move(events));
			}
		}
		static std::optional<FileEventKind> kind_from_mask(
			std::uint32_t mask) {
			if ((mask & IN_Q_OVERFLOW) != 0U) {
				return FileEventKind::overflow;
			}
			if ((mask & IN_CREATE) != 0U) {
				return FileEventKind::created;
			}
			if ((mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) != 0U) {
				return FileEventKind::modified;
			}
			if ((mask & IN_DELETE) != 0U) {
				return FileEventKind::removed;
			}
			if ((mask & IN_MOVED_FROM) != 0U) {
				return FileEventKind::moved_from;
			}
			if ((mask & IN_MOVED_TO) != 0U) {
				return FileEventKind::moved_to;
			}
			return std::nullopt;
		}
		void drain_ready() {
			std::array<char, 16 * 1024> buf{};
			for (;;) {
				ssize_t const n = ::read(fd, buf.data(), buf.size());
				if (n < 0) {
					if (errno == EAGAIN) {
						break;
					}
					emit_error(
						std::make_exception_ptr(std::system_error{errno, std::system_category(), "inotify read"}));
					break;
				}
				if (n == 0) {
					break;
				}
				std::vector<FileEvent> events;
				std::size_t off = 0;
				while (off + sizeof(inotify_event) <= static_cast<std::size_t>(n)) {
					auto const *ev = reinterpret_cast<inotify_event const *>(buf.data() + off);
					std::size_t const step = sizeof(inotify_event) + ev->len;
					if (off + step > static_cast<std::size_t>(n)) {
						break;
					}
					if (auto kind = kind_from_mask(ev->mask)) {
						std::string path = root_for(ev->wd);
						if (ev->len > 0 && ev->name[0] != '\0') {
							if (!path.empty() && path.back() != '/') {
								path += '/';
							}
							path += ev->name;
						}
						events.push_back(
							FileEvent{
								.kind = *kind,
								.path = std::move(path),
								.cookie = ev->cookie,
								.is_directory = (ev->mask & IN_ISDIR) != 0U});
					}
					off += step;
				}
				emit(std::move(events));
			}
		}
	};
	std::shared_ptr<Impl> impl_;

public:
	FileWatcher()
		: impl_{std::make_shared<Impl>()} {
		impl_->fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (impl_->fd < 0) {
			throw std::system_error{errno, std::system_category(), "inotify_init1"};
		}
	}
	~FileWatcher() { stop(); }
	FileWatcher(FileWatcher const &) = delete;
	FileWatcher &operator =(FileWatcher const &) = delete;
	FileWatcher(FileWatcher &&) noexcept = default;
	FileWatcher &operator =(FileWatcher &&) noexcept = default;
	void on_events(
		std::function<void(std::vector<FileEvent>)> cb) {
		std::scoped_lock const lk{impl_->callback_mtx};
		impl_->on_events = std::move(cb);
	}
	void on_error(
		std::function<void(std::exception_ptr)> cb) {
		std::scoped_lock const lk{impl_->callback_mtx};
		impl_->on_error = std::move(cb);
	}
	void watch_directory(
		std::string path,
		WatchOptions options = {}) {
		int const wd = ::inotify_add_watch(impl_->fd, path.c_str(), options.mask);
		if (wd < 0) {
			throw std::system_error{errno, std::system_category(), "inotify_add_watch"};
		}
		{
			std::scoped_lock const lk{impl_->watches_mtx};
			impl_->watches.emplace(wd, std::move(path));
		}
	}
	void start() {
		if (impl_->started.test_and_set()) {
			return;
		}
		auto *reader = current_file_reader();
		if (reader == nullptr) {
			impl_->started.clear();
			throw std::logic_error{"FileWatcher::start requires an active conflux.file_io ring context"};
		}
		auto weak = std::weak_ptr<Impl>{impl_};
		bool const ok = reader->poll_add_multi(impl_->fd, POLLIN, [weak](IoResult r) mutable {
			auto self = weak.lock();
			if (!self || self->stopped.test(std::memory_order_acquire)) {
				return;
			}
			if (r.res < 0) {
				if (r.res != -ECANCELED) {
					self->emit_error(
						std::make_exception_ptr(std::system_error{-r.res, std::system_category(), "inotify poll"}));
				}
				return;
			}
			try {
				self->drain_ready();
			} catch (...) { self->emit_error(std::current_exception()); }
		});
		if (!ok) {
			impl_->started.clear();
			throw std::runtime_error{"FileWatcher::start: io_uring SQ full"};
		}
	}
	// One-shot: start() cannot be called again after stop().
	// To restart, destroy and reconstruct the FileWatcher.
	void stop() noexcept {
		if (!impl_ || impl_->stopped.test_and_set()) {
			return;
		}
		if (impl_->fd >= 0) {
			::close(impl_->fd);
			impl_->fd = -1;
		}
	}
};

} // namespace conflux::file_watch
