// Plain TU — not a module unit.
#include<catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.crypto;
import conflux.net.jwt;
// ---------------------------------------------------------------------------
// jwt_sign (2-arg: default header)
// ---------------------------------------------------------------------------

TEST_CASE(
"jwt: sign and decode round-trip",
"[jwt]"){
S const secret="test-secret-key";
S const payload=R"({"sub":"user1","iss":"test"})";
auto token=jwt_sign(payload,secret);

JwtOptions opts;
opts.secret=secret;
opts.verify_exp=false;
opts.verify_nbf=false;
auto result=jwt_decode(token,opts);
REQUIRE(result.has_value());
CHECK(result->sub=="user1");
CHECK(result->iss=="test");
}
// ---------------------------------------------------------------------------
// jwt_sign (3-arg: custom header)
// ---------------------------------------------------------------------------

TEST_CASE(
"jwt: sign with custom header round-trip",
"[jwt]"){
S const secret="my-secret";
S const header=R"({"alg":"HS256","typ":"JWT","kid":"key-42"})";
S const payload=R"({"sub":"admin","iss":"ghost"})";
auto token=jwt_sign(header,payload,secret);

JwtOptions opts;
opts.secret=secret;
opts.verify_exp=false;
opts.verify_nbf=false;
auto result=jwt_decode(token,opts);
REQUIRE(result.has_value());
CHECK(result->sub=="admin");
CHECK(result->iss=="ghost");
}
TEST_CASE(
"jwt: custom header preserves kid in base64url-decoded header",
"[jwt]"){
S const secret="secret";
S const header=R"({"alg":"HS256","typ":"JWT","kid":"key-99"})";
S const payload=R"({"sub":"x"})";
auto token=jwt_sign(header,payload,secret);

auto dot1=token.find('.');
REQUIRE(dot1!=S::npos);
auto header_b64=SV{token}.substr(0,dot1);
auto decoded_header=base64url_decode(header_b64);
CHECK(decoded_header.find("key-99")!=S::npos);
}
TEST_CASE(
"jwt: wrong secret fails verification",
"[jwt]"){
S const payload=R"({"sub":"u"})";
auto token=jwt_sign(payload,"correct-secret");

JwtOptions opts;
opts.secret="wrong-secret";
opts.verify_exp=false;
auto result=jwt_decode(token,opts);
CHECK(!result.has_value());
}
