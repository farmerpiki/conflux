// P2996 reflection JSON codec — zero boilerplate struct serialization.
// Requires: GCC 16+ with -freflection, CONFLUX_JSON_REFLECT=ON.
import std;
import conflux.types;
import conflux.json;
import conflux.json.reflect;

using namespace conflux::json;
using std::println, std::optional, std::string;
struct Vec3 {
	double x{};
	double y{};
	double z{};
};
struct Material {
	string name;
	double roughness{0.5};
	optional<string> texture;
};
struct SceneObject {
	string id;
	Vec3 position{};
	Material material{};
	[[= conflux::json::skip{}]] int internal_gen{0};
	[[= conflux::json::name("display_name")]] string label;
};
static void example_encode() {
	std::println("--- reflect encode ---");
	SceneObject obj{
		.id = "cube_01",
		.position = {            1.0,              2.5,                  -3.0},
		.material = {.name = "metal", .roughness = 0.2, .texture = "rust.png"},
		.internal_gen = 42,
		.label = "Main Cube",
	};

	ValueBuilder vb;
	auto enc = JsonCodec<SceneObject>::encode(vb, obj);
	if (!enc) {
		std::println("encode error: {}", enc.error().message);
		return;
	}
	auto doc = *move(vb).finish();
	std::println("{}", *doc.dump(JsonDumpOptions{.pretty = true, .indent = 2}));
}
static void example_decode() {
	std::println("\n--- reflect decode ---");
	constexpr std::string_view input = R"({
"id":"sphere_07",
"position":{"x":0,"y":10,"z":0},
"material":{"name":"glass","roughness":0.05},
"display_name":"Glass Sphere"
})";

	auto doc = *parse_view(input);
	auto obj = decode<SceneObject>(doc);
	if (!obj) {
		std::println("decode error: {}", obj.error().message);
		return;
	}
	std::println(
		"id={} pos=({},{},{}) mat={} label={}",
		obj->id,
		obj->position.x,
		obj->position.y,
		obj->position.z,
		obj->material.name,
		obj->label);
	std::println("internal_gen={} (preserved default, skip annotation)", obj->internal_gen);
}
static void example_roundtrip() {
	std::println("\n--- reflect round-trip ---");
	Material orig{.name = "wood", .roughness = 0.8, .texture = "oak.jpg"};

	ValueBuilder vb;
	auto enc = JsonCodec<Material>::encode(vb, orig);
	if (!enc) {
		std::println("encode error: {}", enc.error().message);
		return;
	}
	auto doc = *move(vb).finish();
	auto json_str = *doc.dump();
	std::println("encoded: {}", json_str);

	auto doc2 = *parse_view(json_str);
	auto rt = *decode<Material>(doc2);
	std::println("decoded: name={} roughness={} texture={}", rt.name, rt.roughness, rt.texture.value_or("(none)"));
}
static void example_reader_path() {
	std::println("\n--- reflect decode from JsonReader ---");
	std::string_view input = R"({"x": 1.5, "y": -2.0, "z": 0.0})";
	JsonReader reader{input};
	auto v = decode<Vec3>(reader);
	if (!v) {
		std::println("error: {}", v.error().message);
		return;
	}
	std::println("Vec3 from reader: ({}, {}, {})", v->x, v->y, v->z);
}
int main() {
	example_encode();
	example_decode();
	example_roundtrip();
	example_reader_path();
}
