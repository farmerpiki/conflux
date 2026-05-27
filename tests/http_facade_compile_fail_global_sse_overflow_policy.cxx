// Intentionally invalid: SSE overflow policy lives in conflux::http.
import conflux.http;

auto probe() -> ::SseOverflowPolicy {
	return ::SseOverflowPolicy::DropNewest;
}
