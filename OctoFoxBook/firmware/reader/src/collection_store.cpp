#include "i18n.h"
#include "collection_store.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <cstring>
#include <cstdio>
#include "storage_recovery.h"
#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "network_service.h"
#include "automatic_sync.h"
#endif

namespace {
#ifdef ARDUINO
struct Ram : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p,size_t n) override { return heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
} ram;
JsonDocument state(&ram);
#define DOCUMENT(name) JsonDocument name(&ram)
#else
JsonDocument state;
#define DOCUMENT(name) JsonDocument name
#endif
constexpr const char *path="/reader/collections-v2.json";
constexpr const char *part="/reader/collections-v2.json.part";
constexpr const char *old="/reader/collections-v2.json.old";
char lastError[64]{};
bool loaded=false;
bool fail(const char *message) { snprintf(lastError,sizeof(lastError),"%s",message);return false; }
bool save(JsonDocument &next) {
    if(next.overflowed() || next["shelves"].size()>Collections::kCapacity+1 || measureJson(next)>512*1024) return fail("collection-capacity");
    if(!StorageRecovery::recoverFile(path))return fail("collection-recovery");
    SD.mkdir("/reader"); SD.remove(part);
    File f=SD.open(part,FILE_WRITE);
    const bool written=f && serializeJson(next,f)==measureJson(next);
    f.flush();f.close();
    if(!written) {SD.remove(part);return fail("collection-write");}
    SD.remove(old);
    const bool existed=SD.exists(path);
    if(existed && !SD.rename(path,old)) return fail("collection-backup");
    if(!SD.rename(part,path)) {if(existed)SD.rename(old,path);return fail("collection-publish");}
    SD.remove(old);swap(state,next);loaded=true;lastError[0]=0;return true;
}
void shelf(JsonDocument &d,const char *identity,const char *title) {
    auto c=d["shelves"].as<JsonArray>().add<JsonObject>();
    c["id"]=identity;c["name"]=title;c["books"].to<JsonArray>();
}
void identity(char *out) {
#ifdef ARDUINO
    snprintf(out,33,"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
#else
    static unsigned n=0;snprintf(out,33,"%032x",++n);
#endif
}
bool queue(JsonDocument &d,const char *action,const char *collection,const char *title,const char *book,bool selected) {
    if(d["pending"].size()>=256) return fail("sync-needed");
    char op[33];identity(op);auto p=d["pending"].as<JsonArray>().add<JsonObject>();
    p["op"]=op;p["action"]=action;p["id"]=collection;
    if(title)p["name"]=title;
    if(book){p["book"]=book;p["selected"]=selected;}
    return true;
}
}
namespace Collections {
const char *error(){return lastError;}
bool load() {
    if(loaded)return true;
    if(!StorageRecovery::recoverFile(path))return fail("collection-recovery");
    if(SD.exists(path)) {
        File f=SD.open(path,FILE_READ);
        if(!f || f.size()>512*1024 || deserializeJson(state,f) || state["version"]!=2 || !state["shelves"].is<JsonArray>() || state["shelves"].size()>kCapacity+1 || !state["pending"].is<JsonArray>())return fail("collection-invalid");
        if(strcmp(state["shelves"][0]["id"]|"","favorite"))return fail("collection-invalid");
        loaded=true;return true;
    }
    DOCUMENT(initial);initial["version"]=2;initial["shelves"].to<JsonArray>();initial["pending"].to<JsonArray>();
    shelf(initial,"favorite","Избранное");
    // Preserve the old file and migrate its folders once, with no SD-book deletion.
    if(SD.exists("/reader/favorites-v1.json")) {
        DOCUMENT(legacy);File f=SD.open("/reader/favorites-v1.json",FILE_READ);
        if(!f || deserializeJson(legacy,f) || legacy["version"]!=1 || !legacy["books"].is<JsonArray>())return fail("legacy-favorites-invalid");
        for(JsonObject b:legacy["books"].as<JsonArray>()) {
            const unsigned folder=b["folder"]|1U;const char *bookId=b["id"]|"";
            if(!*bookId)continue;
            const char *cid=folder==1?"favorite":folder==0?"legacy-want":"legacy-later";
            JsonObject target;
            for(JsonObject c:initial["shelves"].as<JsonArray>())if(!strcmp(c["id"]|"",cid))target=c;
            if(target.isNull()) {
                const char *title=folder==0?"Хочу прочитать":"На потом";
                shelf(initial,cid,title);if(!queue(initial,"create",cid,title,nullptr,false))return false;
                target=initial["shelves"][initial["shelves"].size()-1];
            }
            target["books"].as<JsonArray>().add(bookId);
            if(!strncmp(bookId,"opds-",5) && !queue(initial,"member",cid,nullptr,bookId,true))return false;
        }
    }
    return save(initial);
}
size_t count(){return load()?state["shelves"].size():0;}
const char *id(size_t n){return n<count()?state["shelves"][n]["id"]|"":"";}
const char *name(size_t n){return n<count() ? (!strcmp(id(n),"favorite") ? I18n::tr("Избранное") : state["shelves"][n]["name"]|"") : "";}
size_t bookCount(size_t n){return n<count()?state["shelves"][n]["books"].size():0;}
const char *book(size_t n,size_t i){return i<bookCount(n)?state["shelves"][n]["books"][i]|"":"";}
bool contains(size_t n,const char *b){for(size_t i=0;i<bookCount(n);++i)if(!strcmp(book(n,i),b))return true;return false;}
bool create(const char *title) {
    if(!load())return false;
    if(!title || !*title || strspn(title," \t\r\n")==strlen(title) || strlen(title)>240 || count()>kCapacity)return fail("collection-name-or-limit");
    for(size_t n=0;n<count();++n)if(!strcmp(name(n),title))return fail("collection-exists");
    DOCUMENT(next);next.set(state);char cid[33];identity(cid);shelf(next,cid,title);
    return queue(next,"create",cid,title,nullptr,false)&&save(next);
}
bool toggle(size_t n,const char *b) {
    if(!load() || n>=count() || !b || !*b || strlen(b)>32)return fail("invalid-selection");
    DOCUMENT(next);next.set(state);auto books=next["shelves"][n]["books"].as<JsonArray>();
    const bool selected=!contains(n,b);
    if(selected){if(books.size()>=256)return fail("collection-book-limit");books.add(b);}
    else for(size_t i=0;i<books.size();++i)if(!strcmp(books[i]|"",b)){books.remove(i);break;}
    if(!strncmp(b,"opds-",5) && !queue(next,"member",id(n),nullptr,b,selected))return false;
    return save(next);
}

#ifdef ARDUINO
bool sync(NetworkService &network,const char *device,bool uploadOnly) {
    if(!load())return false;
    char url[180];String response;SyncRequestResult result{};
    snprintf(url,sizeof(url),"/reader-api/device/collections?device=%s&offset=0",device);
    if(!network.syncRequest(url,nullptr,response,result))return fail("collections-network");
    DOCUMENT(page);
    if(deserializeJson(page,response) || !page["account"].is<const char*>())return fail("collections-protocol");
    const String account=page["account"].as<const char*>();
    const char *previous=state["account"]|"";
    if(*previous && account!=previous)return fail("collections-account-changed");
    // Bind before the first outbound mutation, including ambiguous responses.
    if(!*previous){DOCUMENT(bound);bound.set(state);bound["account"]=account;if(!save(bound))return false;}
    while(state["pending"].size()) {
        if(AutomaticSync::cancelRequested())return fail("collections-cancelled");
        String request;serializeJson(state["pending"][0],request);
        snprintf(url,sizeof(url),"/reader-api/device/collections?device=%s",device);
        if(!network.syncRequest(url,request.c_str(),response,result))return fail("collections-upload");
        DOCUMENT(ack);if(deserializeJson(ack,response) || !ack["id"].is<const char*>())return fail("collections-protocol");
        DOCUMENT(next);next.set(state);
        const String from=next["pending"][0]["id"].as<const char*>(),to=ack["id"].as<const char*>();
        if(from!=to){
            for(JsonObject c:next["shelves"].as<JsonArray>())if(from==(c["id"]|""))c["id"]=to;
            for(JsonObject op:next["pending"].as<JsonArray>())if(from==(op["id"]|""))op["id"]=to;
        }
        next["pending"].as<JsonArray>().remove(0);if(!save(next))return false;
    }
    if(uploadOnly)return true;
    DOCUMENT(snapshot);snapshot["version"]=2;snapshot["account"]=account;snapshot["pending"].to<JsonArray>();snapshot["shelves"].to<JsonArray>();
    String revision;unsigned offset=0;
    do {
        if(AutomaticSync::cancelRequested())return fail("collections-cancelled");
        snprintf(url,sizeof(url),"/reader-api/device/collections?device=%s&offset=%u",device,offset);
        if(!network.syncRequest(url,nullptr,response,result) || deserializeJson(page,response))return fail("collections-download");
        if(account!=(page["account"]|""))return fail("collections-account-changed");
        String current=page["revision"]|"";
        if(!current.length() || (offset && current!=revision))return fail("collections-changed-retry");
        revision=current;
        for(JsonArray record:page["records"].as<JsonArray>()) {
            const char *kind=record[0]|"",*cid=record[1]|"",*value=record[2]|"";
            if(!strcmp(kind,"s")){if(snapshot["shelves"].size()>kCapacity)return fail("collection-capacity");shelf(snapshot,cid,value);}
            else if(!strcmp(kind,"m")) {
                bool found=false;for(JsonObject c:snapshot["shelves"].as<JsonArray>())if(!strcmp(c["id"]|"",cid)){c["books"].as<JsonArray>().add(value);found=true;break;}
                if(!found)return fail("collections-protocol");
            } else return fail("collections-protocol");
        }
        const unsigned next=page["next"]|0U;if(next && next<=offset)return fail("collections-protocol");offset=next;
        if(offset>30000 || snapshot.overflowed())return fail("collection-capacity");
    } while(offset);
    if(strcmp(snapshot["shelves"][0]["id"]|"","favorite"))return fail("collections-protocol");
    // USB-only books have no server identity. Keep those memberships locally.
    for(JsonObject c:state["shelves"].as<JsonArray>())for(const char *b:c["books"].as<JsonArray>())if(strncmp(b,"opds-",5)) {
        JsonObject target;for(JsonObject x:snapshot["shelves"].as<JsonArray>())if(!strcmp(x["id"]|"",c["id"]|""))target=x;
        if(target.isNull()){shelf(snapshot,c["id"],c["name"]);target=snapshot["shelves"][snapshot["shelves"].size()-1];}
        target["books"].as<JsonArray>().add(b);
    }
    return save(snapshot);
}
#endif
}
