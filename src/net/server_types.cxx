module;

export module conflux.net.http.server_types;

import std;
import conflux.types;
import conflux.net.http.types;

export struct UploadedFile {
	SV name;
	SV filename;
	SV content_type;
	SV data;
	S owned_name;
	S owned_filename;
	S owned_content_type;
	S owned_data;
	bool owns_metadata = false;
	bool owns_data = false;

	UploadedFile() = default;
	UploadedFile(
		SV name_,
		SV filename_,
		SV content_type_,
		SV data_)
		: name(name_)
		, filename(filename_)
		, content_type(content_type_)
		, data(data_) {}
	UploadedFile(
		UploadedFile const &other)
		: owned_name(other.owns_metadata ? other.owned_name : S{})
		, owned_filename(other.owns_metadata ? other.owned_filename : S{})
		, owned_content_type(other.owns_metadata ? other.owned_content_type : S{})
		, owned_data(other.owns_data ? other.owned_data : S{})
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		name = owns_metadata ? SV{owned_name} : other.name;
		filename = owns_metadata ? SV{owned_filename} : other.filename;
		content_type = owns_metadata ? SV{owned_content_type} : other.content_type;
		data = owns_data ? SV{owned_data} : other.data;
	}
	UploadedFile(
		UploadedFile &&other) noexcept
		: name(other.name)
		, filename(other.filename)
		, content_type(other.content_type)
		, data(other.data)
		, owned_name(move(other.owned_name))
		, owned_filename(move(other.owned_filename))
		, owned_content_type(move(other.owned_content_type))
		, owned_data(move(other.owned_data))
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		if (owns_metadata) {
			name = owned_name;
			filename = owned_filename;
			content_type = owned_content_type;
		}
		if (owns_data) {
			data = owned_data;
		}
	}
	UploadedFile &operator =(
		UploadedFile const &other) {
		if (this == &other) {
			return *this;
		}
		owned_name = other.owns_metadata ? other.owned_name : S{};
		owned_filename = other.owns_metadata ? other.owned_filename : S{};
		owned_content_type = other.owns_metadata ? other.owned_content_type : S{};
		owned_data = other.owns_data ? other.owned_data : S{};
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		name = owns_metadata ? SV{owned_name} : other.name;
		filename = owns_metadata ? SV{owned_filename} : other.filename;
		content_type = owns_metadata ? SV{owned_content_type} : other.content_type;
		data = owns_data ? SV{owned_data} : other.data;
		return *this;
	}
	UploadedFile &operator =(
		UploadedFile &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		name = other.name;
		filename = other.filename;
		content_type = other.content_type;
		data = other.data;
		owned_name = move(other.owned_name);
		owned_filename = move(other.owned_filename);
		owned_content_type = move(other.owned_content_type);
		owned_data = move(other.owned_data);
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		if (owns_metadata) {
			name = owned_name;
			filename = owned_filename;
			content_type = owned_content_type;
		}
		if (owns_data) {
			data = owned_data;
		}
		return *this;
	}
	[[nodiscard]] static UploadedFile borrowed(
		SV name_,
		SV filename_,
		SV content_type_,
		SV data_) {
		return UploadedFile{name_, filename_, content_type_, data_};
	}
	[[nodiscard]] static UploadedFile owned(
		S name_,
		S filename_,
		S content_type_,
		S data_) {
		UploadedFile file{SV{}, SV{}, SV{}, SV{}};
		file.owned_name = move(name_);
		file.owned_filename = move(filename_);
		file.owned_content_type = move(content_type_);
		file.owned_data = move(data_);
		file.name = file.owned_name;
		file.filename = file.owned_filename;
		file.content_type = file.owned_content_type;
		file.data = file.owned_data;
		file.owns_metadata = true;
		file.owns_data = true;
		return file;
	}
	[[nodiscard]] UploadedFile to_owned() const {
		if (owns_metadata && owns_data) {
			return *this;
		}
		return UploadedFile::owned(S{name}, S{filename}, S{content_type}, S{data});
	}
};
export struct HttpRequest {
	S method;
	S path; // path only, no query S
	S version;
	S remote_addr; // peer IP address (best-effort with multishot accept)
	bool is_tls = false; // true when request arrived over a TLS connection
	HttpFields params; // {name} captures
	HttpFields headers = HttpFields(true); // case-insensitive lookup
	HttpFields query; // parsed from URL ?k=v&...
	HttpFields form; // parsed from application/x-www-form-urlencoded body or multipart text fields
	HttpFields cookies; // parsed from Cookie: header
	V<UploadedFile> files; // parsed from multipart/form-data body
	S body;
	[[nodiscard]] HttpRequest to_owned() const { return *this; }
};
export struct HttpRequestView {
	SV method;
	SV path;
	SV version;
	SV remote_addr;
	bool is_tls = false;
	HttpFieldsView params;
	HttpFieldsView headers;
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	span<UploadedFile const> files;
	SV body;
	HttpRequestView(
		SV method_,
		SV path_,
		SV version_,
		SV remote_addr_,
		bool is_tls_,
		HttpFieldsView params_,
		HttpFieldsView headers_,
		HttpFieldsView query_,
		HttpFieldsView form_,
		HttpFieldsView cookies_,
		span<UploadedFile const> files_,
		SV body_)
		: method(method_)
		, path(path_)
		, version(version_)
		, remote_addr(remote_addr_)
		, is_tls(is_tls_)
		, params(move(params_))
		, headers(move(headers_))
		, query(move(query_))
		, form(move(form_))
		, cookies(move(cookies_))
		, files(files_)
		, body(body_) {}
	HttpRequestView(
		HttpRequest const &req)
		: method(req.method)
		, path(req.path)
		, version(req.version)
		, remote_addr(req.remote_addr)
		, is_tls(req.is_tls)
		, params(req.params)
		, headers(req.headers)
		, query(req.query)
		, form(req.form)
		, cookies(req.cookies)
		, files(req.files)
		, body(req.body) {}
	[[nodiscard]] HttpRequest to_owned() const {
		HttpRequest owned;
		owned.method = S{method};
		owned.path = S{path};
		owned.version = S{version};
		owned.remote_addr = S{remote_addr};
		owned.is_tls = is_tls;
		owned.params = params.to_owned();
		owned.headers = headers.to_owned();
		owned.query = query.to_owned();
		owned.form = form.to_owned();
		owned.cookies = cookies.to_owned();
		owned.files.reserve(files.size());
		for (auto const &file: files) {
			owned.files.push_back(file.to_owned());
		}
		owned.body = S{body};
		return owned;
	}
};

export template<typename>
class CloneableFunction;
export template<typename R, typename... Args>
class CloneableFunction<R(Args...)> {
	struct Concept {
		virtual ~Concept() = default;
		virtual R invoke(Args... args) = 0;
		[[nodiscard]] virtual UP<Concept> clone() const = 0;
	};
	template<typename F>
	struct Model final : Concept {
		F fn;
		explicit Model(
			F f)
			: fn(move(f)) {}
		R invoke(
			Args... args) override {
			return std::invoke(fn, forward<Args>(args)...);
		}
		[[nodiscard]] UP<Concept> clone() const override { return make_unique<Model>(fn); }
	};
	UP<Concept> fn_{};

public:
	CloneableFunction() = default;
	CloneableFunction(
		std::nullptr_t) {}
	template<typename F>
		requires(!same_as<std::remove_cvref_t<F>, CloneableFunction>)
	CloneableFunction(
		F &&f)
		: fn_(make_unique<Model<std::remove_cvref_t<F>>>(forward<F>(f))) {}
	CloneableFunction(
		CloneableFunction const &o)
		: fn_(o.fn_ ? o.fn_->clone() : nullptr) {}
	CloneableFunction &operator =(
		CloneableFunction const &o) {
		if (this != &o) {
			fn_ = o.fn_ ? o.fn_->clone() : nullptr;
		}
		return *this;
	}
	CloneableFunction(CloneableFunction &&) noexcept = default;
	CloneableFunction &operator =(CloneableFunction &&) noexcept = default;
	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(fn_); }
	R operator ()(
		Args... args) const {
		return fn_->invoke(forward<Args>(args)...);
	}
};
