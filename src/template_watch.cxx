export module conflux.templates.watch;

import std;
import conflux.types;
import conflux.templates;
import conflux.file_watch;

export namespace tmpl::watch {

struct TemplateWatchOptions {
	::WatchOptions file_watch{};
	chrono::milliseconds debounce{50};
	V<S> extensions{".html", ".htm", ".txt"};
	bool reload_on_directory_event = true;
	bool reload_on_overflow = true;
};

class TemplateWatcher {
public:
	TemplateWatcher(Environment &env, S template_dir);
	TemplateWatcher(Environment &env, S template_dir, TemplateWatchOptions options);
	~TemplateWatcher();
	TemplateWatcher(TemplateWatcher const &) = delete;
	TemplateWatcher &operator =(TemplateWatcher const &) = delete;
	TemplateWatcher(TemplateWatcher &&) noexcept = default;
	TemplateWatcher &operator =(TemplateWatcher &&) noexcept = default;

	void on_reload(Fn<void()> cb);
	void on_reload_failed(Fn<void(TemplateBuildReport const &)> cb);
	void on_watch_error(Fn<void(EP)> cb);
	void start();
	void stop() noexcept;
	void request_reload();

private:
	struct Impl;
	SP<Impl> impl_;
};

} // namespace tmpl::watch

namespace tmpl::watch {
namespace fs = std::filesystem;

static bool extension_allowed(
	S const &path,
	V<S> const &extensions) {
	if (extensions.empty()) {
		return true;
	}
	auto const ext = fs::path{path}.extension().string();
	return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

struct TemplateWatcher::Impl {
	Environment *env = nullptr;
	S template_dir;
	TemplateWatchOptions options;
	UP<FileWatcher> watcher;
	jthread worker;
	mutex mtx;
	std::condition_variable_any cv;
	Fn<void()> on_reload_cb;
	Fn<void(TemplateBuildReport const &)> on_reload_failed_cb;
	Fn<void(EP)> on_watch_error_cb;
	bool started = false;
	bool stopped = false;
	bool dirty = false;

	Impl(
		Environment &env_,
		S template_dir_,
		TemplateWatchOptions options_)
		: env{&env_}
		, template_dir{move(template_dir_)}
		, options{move(options_)} {}

	~Impl() { stop(); }

	void emit_reload() {
		Fn<void()> cb;
		{
			SL const lk{mtx};
			cb = on_reload_cb;
		}
		if (cb) {
			cb();
		}
	}

	void emit_reload_failed(
		TemplateBuildReport const &report) {
		Fn<void(TemplateBuildReport const &)> cb;
		{
			SL const lk{mtx};
			cb = on_reload_failed_cb;
		}
		if (cb) {
			cb(report);
		}
	}

	void emit_watch_error(
		EP eptr) {
		Fn<void(EP)> cb;
		{
			SL const lk{mtx};
			cb = on_watch_error_cb;
		}
		if (cb) {
			cb(eptr);
		}
	}

	[[nodiscard]] bool should_reload(
		FileEvent const &ev) const {
		if (ev.kind == FileEventKind::overflow) {
			return options.reload_on_overflow;
		}
		if (ev.is_directory) {
			return options.reload_on_directory_event;
		}
		return extension_allowed(ev.path, options.extensions);
	}

	void mark_dirty() {
		{
			SL const lk{mtx};
			if (stopped) {
				return;
			}
			dirty = true;
		}
		cv.notify_one();
	}

	void reload_loop(
		std::stop_token stop) {
		std::unique_lock lk{mtx};
		for (;;) {
			cv.wait(lk, stop, [this, &stop] { return dirty || stop.stop_requested(); });
			if (stop.stop_requested()) {
				break;
			}
			dirty = false;
			auto const debounce = options.debounce;
			if (debounce.count() > 0) {
				auto deadline = chrono::steady_clock::now() + debounce;
				while (!stop.stop_requested()
					&& cv.wait_until(lk, stop, deadline, [this] { return dirty; })) {
					dirty = false;
					deadline = chrono::steady_clock::now() + debounce;
				}
			}
			if (stop.stop_requested()) {
				break;
			}
			lk.unlock();
			auto result = env->blocking_reload_all_checked();
			if (result) {
				emit_reload();
			} else {
				emit_reload_failed(result.error());
			}
			lk.lock();
		}
	}

	void start(
		SP<Impl> const &self) {
		{
			SL const lk{mtx};
			if (started) {
				return;
			}
			started = true;
			stopped = false;
			dirty = false;
		}
		try {
			auto fw = make_unique<FileWatcher>();
			fw->watch_directory(template_dir, options.file_watch);
			auto weak = weak_ptr<Impl>{self};
			fw->on_events([weak](V<FileEvent> const &events) {
				auto locked = weak.lock();
				if (!locked) {
					return;
				}
				for (auto const &ev: events) {
					if (locked->should_reload(ev)) {
						locked->mark_dirty();
						return;
					}
				}
			});
			fw->on_error([weak](EP eptr) {
				auto locked = weak.lock();
				if (locked) {
					locked->emit_watch_error(move(eptr));
				}
			});
			{
				SL const lk{mtx};
				watcher = move(fw);
				worker = jthread{[weak](std::stop_token stop) {
					auto locked = weak.lock();
					if (locked) {
						locked->reload_loop(stop);
					}
				}};
			}
			watcher->start();
		} catch (...) {
			stop();
			throw;
		}
	}

	void stop() noexcept {
		{
			SL const lk{mtx};
			if (stopped && !started) {
				return;
			}
			stopped = true;
			started = false;
			dirty = false;
		}
		cv.notify_all();
		if (watcher) {
			watcher->stop();
		}
		if (worker.joinable()) {
			worker.request_stop();
			cv.notify_all();
			if (worker.get_id() != std::this_thread::get_id()) {
				worker.join();
			}
		}
		watcher.reset();
	}
};

TemplateWatcher::TemplateWatcher(
	Environment &env,
	S template_dir)
	: TemplateWatcher{env, move(template_dir), {}} {}

TemplateWatcher::TemplateWatcher(
	Environment &env,
	S template_dir,
	TemplateWatchOptions options)
	: impl_{make_shared<Impl>(env, move(template_dir), move(options))} {}

TemplateWatcher::~TemplateWatcher() { stop(); }

void TemplateWatcher::on_reload(
	Fn<void()> cb) {
	SL const lk{impl_->mtx};
	impl_->on_reload_cb = move(cb);
}

void TemplateWatcher::on_reload_failed(
	Fn<void(TemplateBuildReport const &)> cb) {
	SL const lk{impl_->mtx};
	impl_->on_reload_failed_cb = move(cb);
}

void TemplateWatcher::on_watch_error(
	Fn<void(EP)> cb) {
	SL const lk{impl_->mtx};
	impl_->on_watch_error_cb = move(cb);
}

void TemplateWatcher::start() {
	impl_->start(impl_);
}

void TemplateWatcher::stop() noexcept {
	if (impl_) {
		impl_->stop();
	}
}

void TemplateWatcher::request_reload() {
	impl_->mark_dirty();
}

} // namespace tmpl::watch

export namespace conflux::templates::watch {
using ::tmpl::watch::TemplateWatcher;
using ::tmpl::watch::TemplateWatchOptions;
}
