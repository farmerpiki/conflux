// Intentionally invalid: JSON customization points live in conflux::json.
import conflux.json;

struct GlobalJsonProbe {
	int value{};
};

template<>
struct JsonMembers<GlobalJsonProbe> {};
