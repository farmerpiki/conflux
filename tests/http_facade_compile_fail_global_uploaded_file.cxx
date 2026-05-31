// Intentionally invalid: uploaded files live in conflux::http.
import conflux.http;

auto probe() -> ::UploadedFile * {
	return nullptr;
}
