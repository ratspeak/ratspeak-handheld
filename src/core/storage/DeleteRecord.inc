// A small immutable flash tombstone is authoritative over every JSON/audio/
// prepared-envelope copy, including SD media absent when deletion was committed.
// Retain authenticated MID metadata for deduplication; no message body or audio.
namespace {
constexpr size_t DeletedRecordBytes=80;
uint32_t deletedU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1])<<8) | (uint32_t(bytes[2])<<16) | (uint32_t(bytes[3])<<24);
}
void deletedU32(uint8_t* bytes,uint32_t value) {for(unsigned n=0;n<4;++n) bytes[n]=uint8_t(value>>(8*n));}
}
Error MessageTransactions::recordDeletion(const RecordKey& key,bool& deleted,StoredRecordHeader* header) {
    deleted=false;auto flash=medium(0);if(!flash.isReady()) return Error::Unavailable;
    Error error=Error::None;
    for(const char* suffix:{".del",".del.bak"}) {
        char target[128];path(key,0,target,suffix);if(!flash.exists(target)) continue;
        File file=flash.open(target);uint8_t marker[DeletedRecordBytes]{};
        if(!file || file.size()!=sizeof marker || file.read(marker,sizeof marker)!=sizeof marker) {
            error=Error::Read;continue;
        }
        if(memcmp(marker,"DEL1",4) || deletedU32(marker+4)!=key.counter || marker[76]>1 ||
           marker[77]>1 || marker[78]>messaging::LastStatus || marker[79] || bool(marker[76])!=key.incoming ||
           !deletedU32(marker+8) || memcmp(marker+12,key.peer,16)) {error=Error::InvalidRecord;continue;}
        deleted=true;
        if(header) {
            *header={};header->counter=key.counter;header->incoming=key.incoming;header->revision=deletedU32(marker+8);
            memcpy(key.incoming?header->source:header->destination,key.peer,16);
            memcpy(key.incoming?header->destination:header->source,marker+28,16);
            memcpy(header->messageId,marker+44,32);header->hasMessageId=marker[77];header->status=marker[78];
        }
        return Error::None;
    }
    return error;
}
void MessageTransactions::eraseRecord(const Request& request,Result& result) {
    if(!request.key.counter || !request.offset) {result.error=Error::InvalidRecord;return;}
    MessageDocument document;StoredRecordHeader header;
    result.error=load(request.key,document,header);if(result.error!=Error::None) return;
    // Status/read revisions may advance while a confirmation is visible. This
    // exact immutable record is still the intended message; a lower revision or
    // foreign local identity can never be deleted through that stale binding.
    if(header.revision<request.offset || memcmp(header.incoming?header.destination:header.source,request.source,16)) {
        result.error=Error::Stale;return;
    }
    bool other=false;Cursor cursor;beginRecords(cursor,request.key.peer);RecordKey key;
    while(nextRecord(cursor,key)) if(key.counter!=request.key.counter || key.incoming!=request.key.incoming) {other=true;break;}
    if(cursor.error!=Error::None) {result.error=cursor.error;return;}
    uint8_t marker[DeletedRecordBytes]{};memcpy(marker,"DEL1",4);
    deletedU32(marker+4,request.key.counter);deletedU32(marker+8,header.revision);
    memcpy(marker+12,request.key.peer,16);memcpy(marker+28,request.source,16);
    memcpy(marker+44,header.messageId,32);marker[76]=header.incoming;marker[77]=header.hasMessageId;marker[78]=header.status;
    MemorySource source(marker,sizeof marker);
    auto flash=medium(0);char target[128],parent[96];path(request.key,0,target,".del");directory(request.key.peer,0,parent);
    result.flash.attempted=true;
    result.flash.error=result.error=flash.ensureDir(parent)?flash.write(target,source):Error::Write;
    if(result.error!=Error::None) return;
    result.flash.committed=true;result.outcome=Outcome::Committed;result.revision=header.revision;
    result.countDelta=-1;result.unreadDelta=header.incoming && !header.read?-1:0;result.conversationDelta=other?0:-1;
    // Logical deletion is already durable. Cleanup failure cannot expose media
    // again, including after restart or reinserting an old SD card.
    for(unsigned which=0;which<2;++which) {
        auto store=medium(which);if(!store.isReady()) continue;
        for(const char* suffix:{".tmp",".bak","",".audio.tmp",".audio.bak",".audio",".prop.tmp",".prop.bak",".prop"}) {
            path(request.key,which,target,suffix);
            if(store.exists(target)) store.remove(target);
        }
    }
}
