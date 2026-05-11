export module conflux.net.http.json;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.json;
export namespace conflux::http::json{
inline HttpRequest::Builder&set_body(
HttpRequest::Builder&b,
Document const&doc){
auto dumped=doc.dump();
if(dumped)
b.body(move(*dumped));
return b.content_type("application/json");
}
inline HttpRequest::Builder&&set_body(
HttpRequest::Builder&&b,
Document const&doc){
return move(set_body(b,doc));
}
}// namespace conflux::http::json
