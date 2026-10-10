#include "reading_sync.h"
#include "automatic_sync.h"
#include "network_service.h"
#include "reader_pagination.h"
#include "storage_recovery.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

namespace {
struct Ram : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
    void *reallocate(void *p,size_t n) override { return heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
    void deallocate(void *p) override { heap_caps_free(p); }
} ram;
char lastError[64]{};
ReaderSyncPolicy::Error lastPolicy=ReaderSyncPolicy::Error::Protocol;
bool fail(const char *error) { snprintf(lastError,sizeof(lastError),"%s",error);return false; }
bool read(const char *path,JsonDocument &d) {
    if(!StorageRecovery::recoverFile(path))return fail("reading-recovery");
    if(!SD.exists(path))return true;
    File f=SD.open(path,FILE_READ);
    return (f && f.size()<1024*1024 && !deserializeJson(d,f)) || fail("reading-invalid-state");
}
bool save(const char *path,JsonDocument &d) {
    if(d.overflowed() || !StorageRecovery::recoverFile(path))return fail("reading-memory-or-recovery");
    char part[120],old[120];snprintf(part,sizeof(part),"%s.part",path);snprintf(old,sizeof(old),"%s.old",path);
    SD.remove(part);File f=SD.open(part,FILE_WRITE);
    const bool ok=f && serializeJson(d,f)==measureJson(d);f.flush();f.close();
    if(!ok)return fail("reading-write");
    const bool exists=SD.exists(path);
    if(exists && !SD.rename(path,old))return fail("reading-backup");
    if(!SD.rename(part,path)){if(exists)SD.rename(old,path);return fail("reading-publish");}
    SD.remove(old);return true;
}
void identity(char *out,size_t capacity) {
    snprintf(out,capacity,"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
}
bool equal(JsonVariantConst a,JsonVariantConst b) {return a==b;}
bool one(NetworkService &network,const char *device,const char *book,bool uploadOnly) {
    lastPolicy=ReaderSyncPolicy::Error::Protocol;
    char statePath[96],mirrorPath[96],digest[65],url[240];
    snprintf(statePath,sizeof(statePath),"/books/%s/reader-state.json",book);
    snprintf(mirrorPath,sizeof(mirrorPath),"/books/%s/reading-sync.json",book);
    JsonDocument local(&ram),mirror(&ram),remote(&ram),page(&ram);
    if(!read(statePath,local)||!read(mirrorPath,mirror))return false;
    if(uploadOnly && local.isNull() && mirror.isNull())return true;
    if(!NetworkService::verifiedLocalDigest(book,digest))return fail("reading-book-digest");
    String body;SyncRequestResult result;
    auto fetch=[&](unsigned cursor) {
        snprintf(url,sizeof(url),"/reader-api/device/reading?device=%s&book=%s&digest=%s&cursor=%u",device,book+5,digest,cursor);
        if(!network.syncRequest(url,nullptr,body,result)){
            lastPolicy=result.httpCode==409?ReaderSyncPolicy::Error::ReadingState:result.error;
            return fail(result.httpCode==409?"reading-state-conflict":"reading-download");
        }
        page.clear();return !deserializeJson(page,body) || fail("reading-protocol");
    };
    if(!fetch(0)) {
        // A legacy/local OPDS copy may not belong to this server's catalogue.
        // Keep it and its offline progress untouched. Once a sync receipt exists,
        // missing remote state remains an error: never discard pending edits.
        if(result.httpCode==404 && !SD.exists(mirrorPath)) {
            lastError[0]=0;lastPolicy=ReaderSyncPolicy::Error::None;
            return true;
        }
        return false;
    }
    const String account=page["account"]|"";
    if(account.length()!=64 || (mirror["account"].is<const char*>() && account!=(mirror["account"]|"")))return fail("reading-account-changed");
    mirror["account"]=account;
    // Bind account and persist operation IDs BEFORE sending anything. Ambiguous
    // HTTP responses can then be retried without repeating a mutation.
    if(!mirror["pending"].is<JsonArray>())mirror["pending"].to<JsonArray>();
    if(!mirror["pending"].size() && !local.isNull()) {
        if(strcmp(local["schema"]|"","abyss-reader-state") || strcmp(local["book_id"]|"",book))return fail("reading-invalid-state");
        bool assigned=false;
        for(JsonObject mark:local["bookmarks"].as<JsonArray>())if(!mark["id"].is<const char*>()) {
            char id[33];identity(id,sizeof(id));id[24]=0;mark["id"]=id;assigned=true;
        }
        if(assigned && !save(statePath,local))return false;
        auto op=[&](const char *kind) {
            JsonObject o=mirror["pending"].as<JsonArray>().add<JsonObject>();char id[33];identity(id,sizeof(id));
            o["op"]=id;o["kind"]=kind;o["account"]=account;return o;
        };
        if((local["logical"]["record"]|0U)>0 &&
           (!equal(local["logical"],mirror["base"]["position"]) ||
            (local["finished"]|false)!=(mirror["base"]["finished"]|false))) {
            JsonObject o=op("position");o["position"]=local["logical"];o["base"]=mirror["base"]["position"];
            o["finished"]=local["finished"]|false;
        }
        const JsonArrayConst previousMarks=mirror["base"]["bookmarks"].as<JsonArrayConst>();
        for(JsonObject mark:local["bookmarks"].as<JsonArray>()) {
            JsonObjectConst previous;
            for(JsonObjectConst b:previousMarks)if(!strcmp(b["id"]|"",mark["id"]|"")){previous=b;break;}
            if(previous.isNull() || !equal(previous["position"],mark["logical"]) || strcmp(previous["label"]|"",mark["title"]|"")) {
                JsonObject o=op("bookmark");o["id"]=mark["id"];o["position"]=mark["logical"];o["label"]=mark["title"];
            }
        }
        for(JsonObjectConst previous:previousMarks) {
            bool found=false;for(JsonObjectConst b:local["bookmarks"].as<JsonArrayConst>())if(!strcmp(b["id"]|"",previous["id"]|""))found=true;
            if(!found){JsonObject o=op("remove-bookmark");o["id"]=previous["id"];}
        }
    }
    if(!mirror["outgoing"].is<JsonObject>())mirror["outgoing"]=local;
    if(!save(mirrorPath,mirror))return false;
    unsigned acknowledged=mirror["acknowledged"]|0U;
    unsigned operationIndex=0;
    bool sentOperations=false;
    for(JsonObjectConst op:mirror["pending"].as<JsonArrayConst>()) {
        if(operationIndex++<acknowledged)continue;
        if(AutomaticSync::cancelRequested())return fail("reading-cancelled");
        String request;serializeJson(op,request);
        if(request.length()>8000)return fail("reading-operation-too-large");
        if(!network.syncRequest(url,request.c_str(),body,result)){
            lastPolicy=result.httpCode==409?ReaderSyncPolicy::Error::ReadingState:result.error;
            return fail("reading-upload");
        }
        JsonDocument ack(&ram);if(deserializeJson(ack,body) || ack["ok"]!=true)return fail("reading-ack");
        sentOperations=true;
        mirror["acknowledged"]=operationIndex;
        if(!save(mirrorPath,mirror))return false;
    }
    // If a previous pass stopped after uploading, the owner may have kept
    // reading offline. Finish the old receipt, then send those newer edits.
    if(mirror["pending"].size() && !equal(local.as<JsonVariantConst>(),mirror["outgoing"])) {
        JsonVariantConst sent=mirror["outgoing"];
        mirror["base"]["position"]=sent["logical"];
        mirror["base"]["finished"]=sent["finished"]|false;
        auto marks=mirror["base"]["bookmarks"].to<JsonArray>();
        for(JsonObjectConst m:sent["bookmarks"].as<JsonArrayConst>()) {
            auto b=marks.add<JsonObject>();b["id"]=m["id"];b["position"]=m["logical"];b["label"]=m["title"];
        }
        mirror["pending"].to<JsonArray>();mirror.remove("outgoing");mirror.remove("acknowledged");
        if(!save(mirrorPath,mirror))return false;
        return one(network,device,book,uploadOnly);
    }
    // Download even during the pre-removal phase: do not leave stale local
    // copies that could resurrect a removed bookmark on the next pass.
    unsigned cursor=0;String revision;
    remote["bookmarks"].to<JsonArray>();
    do {
        if(AutomaticSync::cancelRequested())return false;
        // The initial GET is already this snapshot's first page. Refetch it only
        // after a mutation; later bookmark pages still verify the same revision.
        if((cursor || sentOperations) && !fetch(cursor))return false;
        String current=page["revision"]|"";
        if(current.length()!=64 || (cursor && revision!=current) || account!=(page["account"]|""))return fail("reading-snapshot-changed");
        revision=current;
        remote["position"]=page["position"];remote["finished"]=page["finished"];
        for(JsonObjectConst m:page["bookmarks"].as<JsonArrayConst>())remote["bookmarks"].as<JsonArray>().add(m);
        unsigned next=page["next"]|0U;if((next && next<=cursor)||remote["bookmarks"].size()>500)return fail("reading-protocol");cursor=next;
    }while(cursor);
    if(!remote["position"].isNull() || remote["bookmarks"].size() || !local.isNull()) {
        JsonDocument merged(&ram);merged.set(local);
        merged["schema"]="abyss-reader-state";merged["version"]=1;merged["book_id"]=book;
        merged["layout"]="sync-v1";merged["current_page"]=1;merged["page_count"]=0;
        if(!remote["position"].isNull())merged["logical"]=remote["position"];
        merged["finished"]=remote["finished"]|false;
        auto marks=merged["bookmarks"].to<JsonArray>();
        for(JsonObjectConst m:remote["bookmarks"].as<JsonArrayConst>()) {
            auto mark=marks.add<JsonObject>();mark["id"]=m["id"];mark["logical"]=m["position"];
            mark["title"]=m["label"];mark["page"]=1;
        }
        if(!save(statePath,merged))return false;
    }
    mirror["base"]=remote;mirror["pending"].to<JsonArray>();mirror.remove("outgoing");mirror.remove("acknowledged");
    return save(mirrorPath,mirror);
}
}
namespace ReadingSync {
const char *error(){return lastError;}
ReaderSyncPolicy::Error policyError(){return lastPolicy;}
bool run(NetworkService &network,const char *device,bool uploadOnly) {
    lastError[0]=0;lastPolicy=ReaderSyncPolicy::Error::Protocol;File root=SD.open("/books",FILE_READ);if(!root)return true;
    const auto stage=uploadOnly?SyncProgress::Stage::ReadingUpload:SyncProgress::Stage::ReadingDownload;
    auto bookName=[](File &f) {
        const bool directory=f.isDirectory();String name=f.name();f.close();
        int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
        if(!directory || !name.startsWith("opds-") || name.length()<=5 || name.length()>32)return String();
        for(unsigned i=5;i<name.length();++i)if(name[i]<'0'||name[i]>'9')return String();
        return name;
    };
    unsigned total=0,completed=0;
    for(;;) {
        if(AutomaticSync::cancelRequested())return fail("reading-cancelled");
        File f=root.openNextFile(FILE_READ);if(!f)break;
        if(bookName(f).length())++total;
    }
    root.close();root=SD.open("/books",FILE_READ);
    AutomaticSync::reportProgress(stage,0,total);
    if(!root)return fail("reading-list-unavailable");
    for(;;) {
        if(AutomaticSync::cancelRequested())return fail("reading-cancelled");
        File f=root.openNextFile(FILE_READ);if(!f)break;
        const String name=bookName(f);if(!name.length())continue;
        if(!one(network,device,name.c_str(),uploadOnly))return false;
        AutomaticSync::reportProgress(stage,++completed,total);
    }
    return true;
}
}
