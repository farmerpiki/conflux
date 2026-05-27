// Intentionally invalid: server run status lives in conflux::http.
import conflux.http;

auto probe() -> RunStatus {
	return RunStatus::stopped_normally;
}
