#include <cassert>
#include <set>
#include "../src/reading_sync.cpp"
FakeSD SD;
namespace AutomaticSync {bool cancelRequested(){return false;}}
JsonDocument server;
std::set<std::string> applied;
bool loseReply=false,failGet=false;
bool NetworkService::verifiedLocalDigest(const char *,char out[65]){memset(out,'d',64);out[64]=0;return true;}
bool NetworkService::syncRequest(const char *,const char *json,String &body,SyncRequestResult &r,uint32_t) {
 r.httpCode=200;body.clear();
 if(!json){if(failGet)return false;serializeJson(server,body);return true;}
 JsonDocument op;assert(!deserializeJson(op,json));
 if(applied.insert(op["op"].as<const char*>()).second) {
  const char *kind=op["kind"];
  if(!strcmp(kind,"position")){server["position"]=op["position"];server["finished"]=op["finished"];}
  else if(!strcmp(kind,"bookmark")) {
   auto m=server["bookmarks"].as<JsonArray>().add<JsonObject>();m["id"]=op["id"];m["position"]=op["position"];m["label"]=op["label"];
  } else if(!strcmp(kind,"remove-bookmark")) {
   auto marks=server["bookmarks"].as<JsonArray>();for(size_t i=0;i<marks.size();++i)if(!strcmp(marks[i]["id"]|"",op["id"]|"")){marks.remove(i);break;}
  }
 }
 if(loseReply){loseReply=false;return false;}
 body="{\"ok\":true}";return true;
}
constexpr auto stateFile="/books/opds-42/reader-state.json";
void localPosition(unsigned record) {
 JsonDocument d;assert(!deserializeJson(d,SD.files[stateFile]));d["logical"]["record"]=record;
 SD.files[stateFile].clear();serializeJson(d,SD.files[stateFile]);
}
int main(){
 server["account"]=std::string(64,'a');server["revision"]=std::string(64,'b');
 server["bookmarks"].to<JsonArray>();server["next"]=0;server["finished"]=false;
 SD.files[stateFile]=R"({"schema":"abyss-reader-state","version":1,"book_id":"opds-42","layout":"test","current_page":1,"page_count":10,"logical":{"record":3,"byte":0},"bookmarks":[{"page":1,"logical":{"record":3,"byte":1},"title":"A bookmark"}]})";
 NetworkService network;
 loseReply=true;assert(!one(network,"reader-test","opds-42",false));
 assert(applied.size()==1); // The server applied it, but its reply was lost.
 localPosition(7); // Owner kept reading before retrying.
 assert(one(network,"reader-test","opds-42",false));
 assert(server["position"]["record"]==7);assert(server["bookmarks"].size()==1);
 const auto mutations=applied.size();assert(one(network,"reader-test","opds-42",false));
 assert(applied.size()==mutations); // Downloaded state is not a fresh local edit.
 JsonDocument saved;assert(!deserializeJson(saved,SD.files[stateFile]));assert(saved["logical"]["record"]==7);
 saved["bookmarks"].to<JsonArray>();SD.files[stateFile].clear();serializeJson(saved,SD.files[stateFile]);
 assert(one(network,"reader-test","opds-42",false));assert(server["bookmarks"].size()==0);
 const auto before=SD.files[stateFile];server["account"]=std::string(64,'c');
 assert(!one(network,"reader-test","opds-42",false));assert(SD.files[stateFile]==before);
 return 0;
}
