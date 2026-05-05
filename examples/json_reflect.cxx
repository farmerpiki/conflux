// P2996 reflection JSON codec — zero boilerplate struct serialization.
// Requires: GCC 16+ with -freflection, CONFLUX_JSON_REFLECT=ON.
import std;
import conflux.types;
import conflux.json;
import conflux.json.reflect;

using namespace conflux::json;
using std::println,std::optional,std::string;
struct Vec3{
double x{};
double y{};
double z{};
};
struct Material{
string name;
double roughness{0.5};
optional<string>texture;
};
struct SceneObject{
string id;
Vec3 position{};
Material material{};
[[=conflux::json::skip{}]]int internal_gen{0};
[[=conflux::json::name("display_name")]]string label;
};
static void example_encode(){
println("--- reflect encode ---");
SceneObject obj{
.id="cube_01",
.position={1.0,2.5,-3.0},
.material={.name="metal",.roughness=0.2,.texture="rust.png"},
.internal_gen=42,
.label="Main Cube",
};

ValueBuilder vb;
auto enc=JsonCodec<SceneObject>::encode(vb,obj);
if(!enc){
println("encode error: {}",enc.error().message);
return;
}
auto doc=*move(vb).finish();
println("{}",*doc.dump(JsonDumpOptions{.pretty=true,.indent=2}));
}
static void example_decode(){
println("\n--- reflect decode ---");
constexpr SV input=R"({
"id":"sphere_07",
"position":{"x":0,"y":10,"z":0},
"material":{"name":"glass","roughness":0.05},
"display_name":"Glass Sphere"
})";

auto doc=*parse(input);
auto obj=decode<SceneObject>(doc);
if(!obj){
println("decode error: {}",obj.error().message);
return;
}
println("id={} pos=({},{},{}) mat={} label={}",
obj->id,obj->position.x,obj->position.y,obj->position.z,
obj->material.name,obj->label);
println("internal_gen={} (preserved default, skip annotation)",obj->internal_gen);
}
static void example_roundtrip(){
println("\n--- reflect round-trip ---");
Material orig{.name="wood",.roughness=0.8,.texture="oak.jpg"};

ValueBuilder vb;
(void)JsonCodec<Material>::encode(vb,orig);
auto doc=*move(vb).finish();
auto json_str=*doc.dump();
println("encoded: {}",json_str);

auto doc2=*parse(json_str);
auto rt=*decode<Material>(doc2);
println("decoded: name={} roughness={} texture={}",
rt.name,rt.roughness,rt.texture.value_or("(none)"));
}
static void example_reader_path(){
println("\n--- reflect decode from JsonReader ---");
SV input=R"({"x": 1.5, "y": -2.0, "z": 0.0})";
JsonReader reader{input};
auto v=decode<Vec3>(reader);
if(!v){
println("error: {}",v.error().message);
return;
}
println("Vec3 from reader: ({}, {}, {})",v->x,v->y,v->z);
}
int main(){
example_encode();
example_decode();
example_roundtrip();
example_reader_path();
}
