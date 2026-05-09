export module conflux.net.rate_limit;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
export struct RateLimitOptions{
// Maximum requests allowed per window.
unsigned requests{100};

// Window duration. Tokens refill fully at each window boundary.
chrono::seconds window{60};

// Maximum burst above the base rate (extra tokens at window start).
// Total capacity = requests + burst.
unsigned burst{0};

// Maximum number of distinct client IPs tracked simultaneously.
// When the limit is reached, the least-recently-seen client is evicted.
// Prevents unbounded memory growth under IP-spoofing DoS.
SZ max_clients{65536};
};
using Clock=chrono::steady_clock;
struct Bucket{
unsigned tokens{};
Clock::time_point window_start{Clock::now()};
};
// LRU-bounded token bucket store. Not exported — module scope to satisfy GCC.
struct RateLimitStore{
explicit RateLimitStore(
SZ max)
:max_(max){}
// Returns a reference to the bucket for `key`, creating it if absent.
// Evicts the least-recently-seen entry when the store is full.
// Transparent hash/equal lets the hot find() path work from SV
// without allocating.
Bucket&touch(
SV key,
unsigned capacity,
Clock::time_point now){
if(auto it=map_.find(key);it!=map_.end()){
// Move to back (most-recently used).
order_.splice(order_.end(),order_,it->second.order_it);
return it->second.bucket;
}
// Evict LRU entry if at capacity.
if(map_.size()>=max_){
map_.erase(order_.front());
order_.pop_front();
}
auto owned=S{key};
order_.push_back(owned);
auto[it,_]=map_.emplace(
move(owned),
Entry{
.bucket=Bucket{.tokens=capacity,.window_start=now},
.order_it=std::prev(order_.end())});
return it->second.bucket;
}
private:
struct TransparentHash{
using is_transparent=void;
SZ operator()(
SV s)const noexcept{
return hash<SV>{}(s);
}
SZ operator()(
S const&s)const noexcept{
return hash<SV>{}(s);
}
};
struct Entry{
Bucket bucket{};
std::list<S>::iterator order_it;
};
SZ max_;
std::list<S>order_;
std::unordered_map<S,Entry,TransparentHash,std::equal_to<>>map_;
};
// Middleware factory: token-bucket rate limiter keyed on remote_addr.
// Thread-safe — shared across all rings via captured SP.
export Router::Middleware rate_limit_middleware(
RateLimitOptions opts={}){
struct State{
mutex mtx;
RateLimitStore store;
explicit State(
SZ max)
:store(max){}
};
auto state=make_shared<State>(max<SZ>(opts.max_clients,1));
unsigned const capacity=opts.requests+opts.burst;

return[opts,capacity,state](HttpRequestView const&req,Router::Handler const&next)->HttpResponse{
auto const now=Clock::now();
auto const key=req.remote_addr.empty()?
S{"unknown"}:
parse_ip(req.remote_addr).transform(ip_to_string).value_or(S{req.remote_addr});

bool allowed=false;
auto retry_after=static_cast<unsigned>(opts.window.count());

{
SL const lock{state->mtx};
auto&bucket=state->store.touch(key,capacity,now);

// Refill: new window → reset tokens to full capacity.
auto elapsed=chrono::duration_cast<chrono::seconds>(now-bucket.window_start);
if(elapsed>=opts.window){
bucket.tokens=capacity;
bucket.window_start=now;
elapsed=chrono::seconds{0};
}

if(bucket.tokens>0){
--bucket.tokens;
allowed=true;
}else{
auto remaining=opts.window-elapsed;
retry_after=static_cast<unsigned>(chrono::duration_cast<chrono::seconds>(remaining).count());
}
}

if(!allowed){
HttpResponse r;
r.status=429;
r.status_text="Too Many Requests";
r.content_type="text/plain; charset=utf-8";
r.set_text_body("Too Many Requests");
r.headers["Retry-After"]=format("{}",retry_after);
return r;
}
return next(req);
};
}
