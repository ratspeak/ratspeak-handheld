// Runs on the existing storage executor. One eviction per job, fixed scratch,
// no retained directory, key vector, extra queue or whole-audio allocation.
void MessageTransactions::retainAudio(const Request& request, Result& result) {
    const auto limit=request.offset;
    if(limit<1 || limit>50) {result.error=Error::InvalidRecord;return;}
    if(_external && _sd && _sd->isReady()) {result.outcome=Outcome::Committed;return;}
    auto removeExpiredBytes=[&](const RecordKey& key) {
        bool removed=removeAudio(key,0);auto flash=medium(0);
        for(const char* suffix:{".prop",".prop.bak",".prop.tmp"}) {
            char candidate[128];path(key,0,candidate,suffix);
            if(flash.exists(candidate) && !flash.remove(candidate)) removed=false;
        }
        return removed;
    };
    MessageDocument document;
    Cursor peers;char peerHex[33];RecordKey oldest;
    uint32_t count=0;
    while(nextPeer(peers,peerHex)) {
        uint8_t peer[16];decodeHex(peerHex,32,peer,16);
        Cursor records;beginRecords(records,peer,&document);records.audioOnly=true;RecordKey key;
        while(nextRecord(records,key)) {
            StoredRecordHeader header;
            result.error=load(key,document,header);
            if(result.error!=Error::None) return; // An unreadable newer record may change ordering.
            if(header.audio.state==3) {
                // Finish interrupted sidecar cleanup before admitting another eviction.
                if(!removeExpiredBytes(key)) {result.error=Error::Write;return;}
                continue;
            }
            if(key.counter==request.key.counter && key.incoming==request.key.incoming && !memcmp(key.peer,request.key.peer,16)) continue;
            if(header.audio.state!=1 || (!header.incoming &&
                (header.status==uint8_t(LXMFStatus::DRAFT) || messaging::pendingStatus(header.status)))) continue;
            ++count;
            const int peerOrder=memcmp(key.peer,oldest.peer,16);
            if(!oldest.counter || key.counter<oldest.counter || (key.counter==oldest.counter &&
                (peerOrder<0 || (!peerOrder && key.incoming<oldest.incoming)))) oldest=key;
            yield();
        }
        if(records.error!=Error::None) {result.error=records.error;return;}
    }
    if(peers.error!=Error::None) {result.error=peers.error;return;}
    if(count<=limit) {result.outcome=Outcome::Committed;return;}
    StoredRecordHeader header;
    result.error=load(oldest,document,header);
    if(result.error!=Error::None) return;
    if(header.revision==UINT32_MAX) {result.error=Error::RevisionExhausted;return;}
    header.audio.state=3;
    if(!recordAudio(document.document(),header.audio)) {result.error=Error::Allocation;return;}
    document.document().remove("prop_prepared");
    document.document()["store_revision"]=++header.revision;
    result.key=oldest;result.revision=header.revision;
    // Only this peer may change, including a partial/failed commit. A no-op
    // sweep must preserve the summaries reduced during startup.
    invalidateSummary(oldest.peer);
    result.error=commit(oldest,document,false,result,nullptr,UINT8_MAX,true);
    if(result.error!=Error::None || !result.flash.committed) return;
    // Publish a durable removed descriptor before destroying any bytes. It also
    // wins over old SD mirrors if that card is reinserted later.
    result.outcome=Outcome::Committed;result.total=1;result.more=count>limit+1;
    if(!removeExpiredBytes(oldest)) result.error=Error::Write;
}
