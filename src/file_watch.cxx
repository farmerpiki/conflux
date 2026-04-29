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

export enum class FileEventKind : u8 {
	created,
	modified,
	removed,
	moved_from,
	moved_to,
	overflow,
};

export struct FileEvent {
	FileEventKind kind{};
	S path{};
	u32 cookie{};
	bool is_directory{};
};

export struct WatchOptions {
	u32 mask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
};

export class FileWatcher {
	struct Impl {
		int fd = -1;
		UM<int, S> watches{};
		Fn<void(V<FileEvent>)> on_events{};
		Fn<void(EP)> on_error{};
		atomic_flag started{};
		atomic_flag stopped{};
		mutex callback_mtx{};
		mutex watches_mtx{};

		~Impl() {
			if (fd >= 0) {
				::close(fd);
				fd = -1;
			}
		}

		[[nodiscard]] S root_for(
			int wd) {
			SL const lk{watches_mtx};
			auto it = watches.find(wd);
			return it != watches.end() ? it->second : S{};
		}

		void emit_error(
			EP eptr) {
			Fn<void(EP)> cb;
			{
				SL const lk{callback_mtx};
				cb = on_error;
			}
			if (cb) {
				cb(eptr);
			}
		}

		void emit(
			V<FileEvent> events) {
			if (events.empty()) {
				return;
			}
			Fn<void(V<FileEvent>)> cb;
			{
				SL const lk{callback_mtx};
				cb = on_events;
			}
			if (cb) {
				cb(move(events));
			}
		}

		static Opt<FileEventKind> kind_from_mask(
			u32 mask) {
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
			A<char, 16 * 1024> buf{};
			for (;;) {
				ssize_t const n = ::read(fd, buf.data(), buf.size());
				if (n < 0) {
					if (errno == EAGAIN) {
						break;
					}
					emit_error(make_exception_ptr(SE{errno, system_category(), "inotify read"}));
					break;
				}
				if (n == 0) {
					break;
				}
				V<FileEvent> events;
				SZ off = 0;
				while (off + sizeof(inotify_event) <= static_cast<SZ>(n)) {
					auto const *ev = reinterpret_cast<inotify_event const *>(buf.data() + off);
					SZ const step = sizeof(inotify_event) + ev->len;
					if (off + step > static_cast<SZ>(n)) {
						break;
					}
					if (auto kind = kind_from_mask(ev->mask)) {
						S path = root_for(ev->wd);
						if (ev->len > 0 && ev->name[0] != '\0') {
							if (!path.empty() && path.back() != '/') {
								path += '/';
							}
							path += ev->name;
						}
						events.push_back(
							FileEvent{
								.kind = *kind,
								.path = move(path),
								.cookie = ev->cookie,
								.is_directory = (ev->mask & IN_ISDIR) != 0U});
					}
					off += step;
				}
				emit(move(events));
			}
		}
	};

	SP<Impl> impl_;

public:
	FileWatcher()
		: impl_{make_shared<Impl>()} {
		impl_->fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (impl_->fd < 0) {
			throw SE{errno, system_category(), "inotify_init1"};
		}
	}

	~FileWatcher() { stop(); }

	FileWatcher(FileWatcher const &) = delete;
	FileWatcher &operator =(FileWatcher const &) = delete;
	FileWatcher(FileWatcher &&) noexcept = default;
	FileWatcher &operator =(FileWatcher &&) noexcept = default;

	void on_events(
		Fn<void(V<FileEvent>)> cb) {
		SL const lk{impl_->callback_mtx};
		impl_->on_events = move(cb);
	}

	void on_error(
		Fn<void(EP)> cb) {
		SL const lk{impl_->callback_mtx};
		impl_->on_error = move(cb);
	}

	void watch_directory(
		S path,
		WatchOptions options = {}) {
		int const wd = ::inotify_add_watch(impl_->fd, path.c_str(), options.mask);
		if (wd < 0) {
			throw SE{errno, system_category(), "inotify_add_watch"};
		}
		{
			SL const lk{impl_->watches_mtx};
			impl_->watches.emplace(wd, move(path));
		}
	}

	void start() {
		if (impl_->started.test_and_set()) {
			return;
		}
		auto *reader = current_file_reader();
		if (reader == nullptr) {
			impl_->started.clear();
			throw LE{"FileWatcher::start requires an active conflux.file_io ring context"};
		}
		auto weak = weak_ptr<Impl>{impl_};
		bool const ok = reader->poll_add_multi(impl_->fd, POLLIN, [weak](IoResult r) mutable {
			auto self = weak.lock();
			if (!self || self->stopped.test(memory_order_acquire)) {
				return;
			}
			if (r.res < 0) {
				if (r.res != -ECANCELED) {
					self->emit_error(make_exception_ptr(SE{-r.res, system_category(), "inotify poll"}));
				}
				return;
			}
			try {
				self->drain_ready();
			} catch (...) { self->emit_error(current_exception()); }
		});
		if (!ok) {
			impl_->started.clear();
			throw RE{"FileWatcher::start: io_uring SQ full"};
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
