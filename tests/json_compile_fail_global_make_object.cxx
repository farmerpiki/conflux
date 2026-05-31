import conflux.json;

int main() {
	auto doc = make_object(std::pair{"ok", true});
	return doc.has_value() ? 0 : 1;
}
