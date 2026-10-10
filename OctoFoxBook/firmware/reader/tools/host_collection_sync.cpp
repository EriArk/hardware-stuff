#include <ArduinoJson.h>
#define ARDUINO 1
#include "../src/collection_store.cpp"
#include <cassert>
FakeSD SD;
namespace AutomaticSync { bool cancelRequested(){return false;} }
bool failNetwork=true;
ReaderSyncPolicy::Error networkError=ReaderSyncPolicy::Error::Wifi;
int networkCode=0;
unsigned posts=0;
bool NetworkService::syncRequest(const char *,const char *json,String &body,SyncRequestResult &result,uint32_t) {
    if(json)++posts;
    result.httpCode=networkCode;result.error=networkError;
    if(failNetwork)return false;
    body=R"({"account":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","revision":"b","records":[["s","favorite","Favorites"]],"next":0})";
    return true;
}
int main() {
    NetworkService network;
    assert(Collections::load());
    const auto before=SD.files.at("/reader/collections-v2.json");
    assert(!Collections::sync(network,"reader-test",true));
    assert(Collections::policyError()==ReaderSyncPolicy::Error::Wifi);
    assert(Collections::httpCode()==0);
    assert(!strcmp(Collections::error(),"collections-network"));
    assert(SD.files.at("/reader/collections-v2.json")==before);
    networkError=ReaderSyncPolicy::Error::Authentication;networkCode=401;
    assert(!Collections::sync(network,"reader-test",true));
    assert(Collections::policyError()==ReaderSyncPolicy::Error::Authentication);
    assert(Collections::httpCode()==401);
    failNetwork=false;networkCode=200;
    assert(Collections::sync(network,"reader-test",true));
    // A previous account must never be silently adopted or uploaded into another.
    state["account"]="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    assert(!Collections::sync(network,"reader-test",true));
    assert(!strcmp(Collections::error(),"collections-account-changed"));
    assert(Collections::policyError()==ReaderSyncPolicy::Error::Protocol);
    assert(Collections::httpCode()==200 && posts==0);
}
