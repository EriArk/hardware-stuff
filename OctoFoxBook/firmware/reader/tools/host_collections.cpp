#include "collection_store.h"
#include "SD.h"
#include <cassert>
#include <cstring>
#include <iostream>
FakeSD SD;
int main(int argc,char**) {
    const char *path="/reader/collections-v2.json";
    if(argc>1) {
        SD.files[path]="{bad";
        assert(!Collections::load());assert(!Collections::create("Do not overwrite"));
        assert(SD.files[path]=="{bad");return 0;
    }
    SD.files["/reader/favorites-v1.json"]=R"({"version":1,"books":[{"id":"opds-42","folder":1},{"id":"usb-book","folder":0}]})";
    assert(Collections::load() && Collections::count()==2);
    assert(Collections::contains(0,"opds-42"));assert(Collections::contains(1,"usb-book"));
    assert(Collections::create("Русская коллекция"));
    assert(Collections::toggle(2,"opds-42"));
    assert(Collections::contains(0,"opds-42") && Collections::contains(2,"opds-42"));
    assert(Collections::toggle(0,"opds-42") && !Collections::contains(0,"opds-42"));
    assert(Collections::contains(2,"opds-42"));
    assert(!Collections::create("Русская коллекция"));assert(!Collections::create("  "));
    const std::string previous=SD.files.at(path);
    SD.failingRename=std::string(path)+".part";
    assert(!Collections::toggle(2,"opds-43"));assert(!Collections::contains(2,"opds-43"));
    assert(SD.files.at(path)==previous);
    SD.failingRename.clear();assert(Collections::toggle(2,"opds-43"));
    assert(Collections::contains(2,"opds-43"));
    assert(SD.exists("/reader/favorites-v1.json"));
    assert(!SD.exists("/reader/collections-v2.json.old"));
    assert(!SD.exists("/reader/collections-v2.json.part"));
    const auto current=SD.files.at(path);
    SD.files["/sync-receipt.json"]="old lease";
    SD.failingRename=path;
    assert(!Collections::archiveProfile());
    assert(SD.files.at(path)==current && SD.exists("/reader/favorites-v1.json"));
    assert(SD.exists("/sync-receipt.json"));
    SD.failingRename.clear();
    assert(Collections::archiveProfile());
    assert(!SD.exists(path) && !SD.exists("/reader/favorites-v1.json"));
    assert(!SD.exists("/sync-receipt.json"));
    assert(SD.files.at("/trash/profile-0/collections-v2.json")==current);
    assert(Collections::load() && Collections::count()==1 && Collections::bookCount(0)==0);
    std::cout<<"COLLECTIONS_OK\n";
}
