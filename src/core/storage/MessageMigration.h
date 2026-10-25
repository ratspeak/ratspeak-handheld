#pragma once

#include "MessageRecord.h"
#include "MessageTransactions.h"
#include "FlashStore.h"
#include "SDStore.h"
#include "config/Config.h"
#include <Preferences.h>

namespace handheld::storage {

// Boot-only, under the filesystem lease and before queue admission. Listings
// reopen directories for each bounded batch of lexical successors, keeping
// memory independent of history. Legacy names retain their numeric counter:
// mirrored donor bytes then resolve to the same key, and .read_ctr keeps its
// original ordering meaning. An item that cannot be proven or copied stays
// where it is for a later boot; only an unlistable message root stops a medium.
class MessageMigration {
    struct Medium {
        FlashStore* flash = nullptr;
        SDStore* sd = nullptr;
        bool ready() const { return flash ? flash->isReady() : sd && sd->isReady(); }
        File open(const char* p) { return flash ? flash->openFile(p) : sd ? sd->openFile(p) : File(); }
        bool exists(const char* p) { return flash ? flash->exists(p) : sd && sd->exists(p); }
        bool remove(const char* p) { return flash ? flash->remove(p) : sd && sd->remove(p); }
        bool rename(const char* a, const char* b) { return flash ? flash->rename(a,b) : sd && sd->rename(a,b); }
        bool mkdir(const char* p) { return flash ? flash->ensureDir(p) : sd && sd->ensureDir(p); }
        bool rmdir(const char* p) { return flash ? flash->removeDir(p) : sd && sd->removeDir(p); }
        bool write(const char* p, const AtomicSource& source) {
            return (flash ? flash->writeAtomic(p,source) : sd ? sd->writeAtomic(p,source) : Error::Unavailable) == Error::None;
        }
    };
    class FileSource final : public AtomicSource {
        File& _file;
        size_t _length;
    public:
        explicit FileSource(File& file) : _file(file), _length(file.size()) {}
        size_t length() const override { return _length; }
        bool emit(ByteSink& sink) const override {
            if (!_file.seek(0)) return false;
            uint8_t bytes[512]; size_t remaining = _length;
            while (remaining) {
                const size_t count = std::min(remaining,sizeof(bytes));
                if (_file.read(bytes,count) != count || sink.write(bytes,count) != count) return false;
                remaining -= count; yield();
            }
            return true;
        }
    };
    struct Name {
        uint32_t counter = 0;
        bool incoming = false, canonical = false, backup = false;
    };
    // Retry: may succeed on a later boot (I/O, memory). Skipped: these bytes never will.
    enum class Item : uint8_t { Routed, Skipped, Retry };
    static constexpr size_t Batch = 8;
    using Names = char[Batch][40];
    FlashStore* _flash;
    SDStore* _sd;
    bool _external;
    bool _scanFailed = false;
    uint32_t _floor = 0;
    unsigned _skipped = 0;
    Medium medium(unsigned n) const { return n ? Medium{nullptr,_external ? _sd : nullptr} : Medium{_flash,nullptr}; }
    static const char* root(unsigned n) { return n ? SD_PATH_MESSAGES : PATH_MESSAGES; }
    static const char* basename(const char* path) { const char* slash = strrchr(path,'/'); return slash ? slash+1 : path; }
    static bool join(const char* directory, const char* name, char (&path)[128]) {
        const int size = snprintf(path,sizeof(path),"%s/%s",directory,name);
        return size > 0 && size < int(sizeof(path));
    }
    static bool decimal(const char* text, size_t length, uint32_t& value) {
        if (!length || length > 13) return false;
        uint64_t parsed = 0;
        for (size_t i=0;i<length;++i) {
            if (text[i]<'0' || text[i]>'9') return false;
            parsed = parsed*10+unsigned(text[i]-'0');
            if (parsed>UINT32_MAX) return false;
        }
        value=uint32_t(parsed); return true;
    }
    static bool messageLike(const char* name) {
        const size_t length=strlen(name);
        const size_t end=length>=4 && !strcmp(name+length-4,".bak") ? length-4 : length;
        return end>=7 && (!strncmp(name+end-7,"_i.json",7) || !strncmp(name+end-7,"_o.json",7));
    }
    static bool filename(const char* name, Name& out) {
        size_t length=strlen(name); out={};
        if (length>=4 && !strcmp(name+length-4,".bak")) { out.backup=true; length-=4; }
        if (length<8 || length>20 || name[length-7]!='_' ||
            (name[length-6]!='i' && name[length-6]!='o') || strncmp(name+length-5,".json",5) ||
            !decimal(name,length-7,out.counter) || !out.counter) return false;
        out.incoming=name[length-6]=='i'; out.canonical=length==20; return true;
    }
    // One pass keeps the next Batch names after `after`, in order. Names come
    // from readdir: the SDK's File per entry costs a stat and an open, each a
    // linear FAT directory scan, for every entry visited.
    size_t next(Medium store, const char* directory, const char* after, Names& out, bool dirs) {
        File dir=store.open(directory); size_t count=0;
        if (!dir || !dir.isDirectory()) { if (store.exists(directory)) _scanFailed = true; return 0; }
        bool isDir=false;
        for (String entry=dir.getNextFileName(&isDir);entry.length();entry=dir.getNextFileName(&isDir)) {
            const char* name=basename(entry.c_str()); const size_t length=strlen(name);
            if (isDir!=dirs || length>=sizeof(out[0]) || strcmp(name,after)<=0) continue;
            size_t at=count; while (at && strcmp(name,out[at-1])<0) --at;
            if (at==Batch) continue;
            if (count<Batch) ++count;
            for (size_t i=count-1;i>at;--i) memcpy(out[i],out[i-1],sizeof(out[0]));
            memcpy(out[at],name,length+1);
        }
        return count;
    }
    static bool equal(Medium store, const char* a, Medium other, const char* b) {
        File source=store.open(a), target=other.open(b);
        if (!source || !target || source.size()!=target.size()) return false;
        FileSource bytes(source); uint8_t scratch[Budget::IoScratch];
        FileSink sink(target,scratch,bytes.length(),true);
        return bytes.emit(sink) && sink.finish();
    }
    static bool equal(Medium store, const char* a, const char* b) { return equal(store,a,store,b); }
    class SliceSink final : public ByteSink {
        uint8_t* _bytes;
        size_t _offset, _capacity, _position = 0;
    public:
        SliceSink(uint8_t* bytes,size_t offset,size_t capacity) : _bytes(bytes),_offset(offset),_capacity(capacity) {}
        size_t write(const uint8_t* bytes,size_t length) override {
            const size_t first=std::max(_position,_offset),last=std::min(_position+length,_offset+_capacity);
            if (last>first) memcpy(_bytes+(first-_offset),bytes+(first-_position),last-first);
            _position+=length; return length;
        }
    };
    static bool normalized(Medium store,const char* path,MessageDocument& document,uint32_t& revision,size_t& length,double* timestamp = nullptr) {
        StoredRecordHeader record;
        if (header(store,path,document,record)!=Error::None) return false;
        revision=record.revision;
        if (timestamp) *timestamp=record.timestamp;
        // Unknown schema remains byte-verified only. Do not collapse unknown
        // numeric values through serializer rounding while comparing revisions.
        for (JsonPairConst member : document.document().as<JsonObjectConst>()) {
            const char* key=member.key().c_str();bool known=false;
            for (const char* allowed : {"src","dst","ts","title","content","incoming","read","status","store_revision","rcv","msgid"})
                if (!strcmp(key,allowed)) { known=true;break; }
            if (!known) return false;
        }
        if ((!document.document()["ts"].isNull() && !document.document()["ts"].is<double>()) ||
            (!document.document()["rcv"].isNull() && !document.document()["rcv"].is<uint32_t>())) return false;
        document.document().remove("store_revision");
        document.document().remove("status");
        document.document().remove("read");
        JsonSource source(document); length=source.length(); return source.error()==Error::None;
    }
    // Copies may differ in the three mutable storage fields, even at one revision:
    // legacy rsCardputer rewrote status on each medium separately. Compare
    // every other known field, including exact timestamp values,
    // with one parser arena and 512 bytes of fixed slice scratch. This slower fallback is
    // only used when ordinary byte comparison differs; no second body/document
    // or probabilistic hash is used to authorize source removal.
    static bool sameContent(Medium sourceStore,const char* sourcePath,Medium targetStore,const char* targetPath) {
        MessageDocument document; uint32_t sourceRevision=0,targetRevision=0;size_t sourceLength=0,targetLength=0;
        double sourceTime=0,targetTime=0;
        if (!normalized(sourceStore,sourcePath,document,sourceRevision,sourceLength,&sourceTime) ||
            !normalized(targetStore,targetPath,document,targetRevision,targetLength,&targetTime) ||
            sourceLength!=targetLength || sourceTime!=targetTime) return false;
        uint8_t sourceBytes[256],targetBytes[256];
        for (size_t offset=0;offset<sourceLength;offset+=sizeof(sourceBytes)) {
            const size_t count=std::min(sizeof(sourceBytes),sourceLength-offset);
            uint32_t revision=0;size_t length=0;
            if (!normalized(sourceStore,sourcePath,document,revision,length) || revision!=sourceRevision || length!=sourceLength) return false;
            JsonSource source(document);SliceSink sourceSink(sourceBytes,offset,count);
            if (!source.emit(sourceSink)) return false;
            if (!normalized(targetStore,targetPath,document,revision,length) || revision!=targetRevision || length!=targetLength) return false;
            JsonSource target(document);SliceSink targetSink(targetBytes,offset,count);
            if (!target.emit(targetSink) || memcmp(sourceBytes,targetBytes,count)) return false;
            yield();
        }
        return true;
    }
    bool otherCopy(unsigned which,const char* peer,const char* canonical,const char* source) {
        auto other=medium(1-which); if (!other.ready()) return true;
        // Compare the logical key's primary AND backup even when this source
        // itself is a backup; never accidentally probe .bak.bak as its primary.
        char primary[32];const size_t length=strnlen(canonical,sizeof(primary));
        if (length>=sizeof(primary)) return false;
        memcpy(primary,canonical,length+1);
        if (length>=4 && !strcmp(primary+length-4,".bak")) primary[length-4]=0;
        for (const char* suffix : {"",".bak"}) {
            char candidate[128]; snprintf(candidate,sizeof(candidate),"%s/%s/%s%s",root(1-which),peer,primary,suffix);
            if (other.exists(candidate) && !equal(medium(which),source,other,candidate) &&
                !sameContent(medium(which),source,other,candidate)) return false;
        }
        return true;
    }
    // Never overwrite a surviving target or its rollback copy: a different
    // record there is another message. A copy verified before a cut may since
    // have gained read or status state; it still proves the source redundant.
    static Item move(Medium store, const char* source, const char* target) {
        if (!strcmp(source,target)) return Item::Routed;
        bool copied=false;
        if (!store.exists(target)) {
            char backup[128]; const int n=snprintf(backup,sizeof(backup),"%s.bak",target);
            if (n<0 || n>=int(sizeof(backup))) return Item::Skipped;
            if (store.exists(backup)) {
                if (!equal(store,source,backup) && !sameContent(store,source,store,backup)) return Item::Skipped;
                if (!store.rename(backup,target)) return Item::Retry;
            } else {
                File file=store.open(source); if (!file) return Item::Retry;
                FileSource bytes(file); if (!store.write(target,bytes)) return Item::Retry;
                copied=true;
            }
        }
        if (!equal(store,source,target)) {
            if (copied) { store.remove(target); return Item::Retry; }
            if (!sameContent(store,source,store,target)) return Item::Skipped;
        }
        return store.remove(source) ? Item::Routed : Item::Retry;
    }
    // Valid copies raise value. An unparsable copy never lowers a cutoff: it is
    // reported through corrupt and left in place. False: an existing copy could not be read.
    static bool marker(Medium store, const char* directory, const char* name, bool json,
                       uint32_t& value, bool& found, bool* backupBest = nullptr, bool* corrupt = nullptr) {
        value=0; found=false;
        if (backupBest) *backupBest=false;
        if (corrupt) *corrupt=false;
        bool primaryFound=false;
        for (const char* suffix : {"",".bak"}) {
            char path[128]; const int n=snprintf(path,sizeof(path),"%s/%s%s",directory,name,suffix);
            if (n<0 || n>=int(sizeof(path))) return false;
            if (!store.exists(path)) continue;
            File file=store.open(path); if (!file) return false;
            uint32_t current=0; bool valid=false;
            if (json) {
                MessageDocument document;
                const auto parsed=file.size()>512 ? Error::InvalidRecord : document.parse(file);
                if (parsed==Error::Read || parsed==Error::Allocation) return false;
                valid=parsed==Error::None && document.document()["through"].is<uint32_t>();
                if (valid) current=document.document()["through"].as<uint32_t>();
            } else {
                char bytes[32]; const size_t size=file.size();
                if (size && size<sizeof(bytes)) {
                    if (file.read(reinterpret_cast<uint8_t*>(bytes),size)!=size) return false;
                    size_t end=size; while (end && (bytes[end-1]=='\n' || bytes[end-1]=='\r' || bytes[end-1]==' ')) --end;
                    valid=decimal(bytes,end,current);
                }
            }
            if (!valid) { if (corrupt) *corrupt=true; continue; }
            found=true;
            if (!suffix[0]) primaryFound=true;
            else if (backupBest && (!primaryFound || current>value)) *backupBest=true;
            value=std::max(value,current);
        }
        return true;
    }
    static bool removeMarker(Medium store,const char* directory,const char* name) {
        for (const char* suffix : {".bak",""}) {
            char path[128]; snprintf(path,sizeof(path),"%s/%s%s",directory,name,suffix);
            if (store.exists(path) && !store.remove(path)) return false;
        }
        return true;
    }
    static bool mergeMarker(Medium store,const char* directory,const char* name,bool json,uint32_t value) {
        uint32_t current=0; bool found=false,backupBest=false,corrupt=false;
        // An unreadable marker may hold a higher cutoff; never overwrite it.
        if (!marker(store,directory,name,json,current,found,&backupBest,&corrupt) || corrupt) return false;
        if (found && current>=value) return true;
        char path[128]; if (!join(directory,name,path)) return false;
        // Preserve the newest backup before the ordinary atomic writer can
        // rotate an older primary over it, just as semantic transactions do.
        char backup[128]; snprintf(backup,sizeof(backup),"%s.bak",path);
        if (backupBest) {
            if (store.exists(path) && !store.remove(path)) return false;
            if (!store.rename(backup,path)) return false;
        }
        if (json) {
            MessageDocument document; document.document()["through"]=value;
            JsonSource source(document); return source.error()==Error::None && store.write(path,source);
        }
        char bytes[16]; const int length=snprintf(bytes,sizeof(bytes),"%lu",(unsigned long)value);
        MemorySource source(reinterpret_cast<const uint8_t*>(bytes),size_t(length)); return store.write(path,source);
    }
    bool deleted(const char* fullPeer) const {
        char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",PATH_MESSAGES,fullPeer);
        auto flash=medium(0);
        for (const char* suffix : {".deleted",".deleted.bak"}) {
            char path[128]; if (!join(directory,suffix,path) || flash.exists(path)) return true;
        }
        return false;
    }
    // InvalidRecord is a property of the bytes; any other error may clear by a later boot.
    static Error header(Medium store,const char* path,MessageDocument& document,StoredRecordHeader& out) {
        File file=store.open(path); out={};
        if (!file) return Error::Read;
        const auto error=document.parse(file);
        if (error!=Error::None) return error;
        return recordHeader(document.document(),out) ? Error::None : Error::InvalidRecord;
    }
    void notImported(const char* path, const char* reason) {
        if (++_skipped<=16) Serial.printf("[MSGSTORE] Legacy item not imported (%s): %s\n", reason, path);
    }
    // A counter that is or may later be imported is never issued to a new message.
    Item retry(const char* path, uint32_t counter) { notImported(path,"retry"); _floor=std::max(_floor,counter); return Item::Retry; }
    Item route(unsigned which,const char* shortPeer,const char* directory,const char* name,
               uint32_t readThrough,bool haveRead,uint32_t deleteThrough,bool haveDelete) {
        auto store=medium(which); char source[128]; Name parsed;
        if (!join(directory,name,source)) return Item::Skipped;
        if (!filename(name,parsed) || (!parsed.canonical && !LEGACY_MSG_FILENAME_MIGRATION)) {
            notImported(source,"name"); return Item::Skipped;
        }
        MessageDocument document; StoredRecordHeader record;
        const auto error=header(store,source,document,record);
        if (error!=Error::None && error!=Error::InvalidRecord) return retry(source,parsed.counter);
        if (error!=Error::None || record.incoming!=parsed.incoming) { notImported(source,"record"); return Item::Skipped; }
        char fullPeer[33]; encodeHex(parsed.incoming ? record.source : record.destination,16,fullPeer);
        if (strncmp(fullPeer,shortPeer,16) || (!parsed.canonical && (haveDelete || deleted(fullPeer)))) {
            notImported(source,"peer"); return Item::Skipped;
        }
        document.reset(); char targetDir[128],target[128];
        snprintf(targetDir,sizeof(targetDir),"%s/%s",root(which),fullPeer);
        if (!join(targetDir,name,target)) return Item::Skipped;
        if (parsed.canonical && !otherCopy(which,fullPeer,name,source)) { notImported(source,"conflict"); return Item::Skipped; }
        if (!store.mkdir(targetDir) || (haveRead && !mergeMarker(store,targetDir,".read_ctr",false,readThrough)))
            return retry(source,parsed.counter);
        if (haveDelete) {
            char flashDir[128]; snprintf(flashDir,sizeof(flashDir),"%s/%s",PATH_MESSAGES,fullPeer);
            auto flash=medium(0);
            if (!flash.mkdir(flashDir) || !mergeMarker(flash,flashDir,".deleted",true,deleteThrough)) return retry(source,parsed.counter);
        }
        const auto moved=move(store,source,target);
        if (moved==Item::Retry) return retry(source,parsed.counter);
        if (moved==Item::Skipped) notImported(source,"conflict");
        else _floor=std::max(_floor,parsed.counter);
        return moved;
    }
    void truncated(unsigned which,const char* shortPeer) {
        auto store=medium(which); char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),shortPeer);
        uint32_t readThrough=0,deleteThrough=0; bool haveRead=false,haveDelete=false,badDelete=false;
        const bool readable=marker(store,directory,".read_ctr",false,readThrough,haveRead) &&
            marker(store,directory,".deleted",true,deleteThrough,haveDelete,nullptr,&badDelete);
        // Without a readable deletion cutoff, importing could resurrect deleted
        // history. Held items still bound the counter for a later import.
        const bool held=!readable || badDelete;
        if (held) notImported(directory,readable ? "marker" : "retry");
        // Markers reach each routed record's conversation before it moves, and
        // retire once nothing left here can be routed by a later boot.
        bool seen=false,routed=false,retrying=false; char after[40]={}; Names names;
        for (size_t count;(count=next(store,directory,after,names,false));) {
            memcpy(after,names[count-1],sizeof(after));
            for (size_t i=0;i<count;++i) {
                if (!messageLike(names[i])) continue;
                Name parsed; seen=true;
                if (held) { if (filename(names[i],parsed)) _floor=std::max(_floor,parsed.counter); continue; }
                const auto item=route(which,shortPeer,directory,names[i],readThrough,haveRead,deleteThrough,haveDelete);
                routed|=item==Item::Routed; retrying|=item==Item::Retry; yield();
            }
        }
        if (_scanFailed) { _scanFailed=false; notImported(directory,"retry"); return; }
        if (held) return;
        if (routed ? !retrying : !seen) { removeMarker(store,directory,".read_ctr"); removeMarker(store,directory,".deleted"); }
        else if (haveDelete && !retrying) notImported(directory,"deletion");
        store.rmdir(directory); // Empty remnants are harmless.
    }
    void filenames(unsigned which,const char* peer) {
        auto store=medium(which); char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),peer);
        char after[40]={}; Names names;
        for (size_t count;(count=next(store,directory,after,names,false));) {
            memcpy(after,names[count-1],sizeof(after));
            for (size_t i=0;i<count;++i) {
                const char* name=names[i]; char source[128]; Name parsed;
                if (!messageLike(name) || !join(directory,name,source)) continue;
                if (!filename(name,parsed)) { notImported(source,"name"); continue; }
                if (parsed.canonical) continue;
                // Earlier donor migration renumbered files. Without a proven old
                // mapping, assigning this late import past a deletion cutoff would
                // resurrect history. Keep the source for explicit recovery instead.
                if (deleted(peer)) { notImported(source,"peer"); continue; }
                MessageDocument document; StoredRecordHeader record;
                const auto error=header(store,source,document,record);
                if (error!=Error::None && error!=Error::InvalidRecord) { retry(source,parsed.counter); continue; }
                char fullPeer[33]={};
                if (error==Error::None) encodeHex(parsed.incoming ? record.source : record.destination,16,fullPeer);
                if (error!=Error::None || record.incoming!=parsed.incoming || strcmp(peer,fullPeer)) {
                    notImported(source,"record"); continue;
                }
                document.reset(); char target[128],canonical[32];
                snprintf(canonical,sizeof(canonical),"%013lu_%c.json%s",(unsigned long)parsed.counter,parsed.incoming?'i':'o',parsed.backup?".bak":"");
                if (!otherCopy(which,peer,canonical,source) || !join(directory,canonical,target)) {
                    notImported(source,"conflict"); continue;
                }
                const auto moved=move(store,source,target);
                if (moved==Item::Retry) retry(source,parsed.counter);
                else if (moved==Item::Skipped) notImported(source,"conflict");
                else _floor=std::max(_floor,parsed.counter);
                yield();
            }
        }
        if (_scanFailed) { _scanFailed=false; notImported(directory,"retry"); }
    }
public:
    MessageMigration(FlashStore* flash,SDStore* sd,bool external) : _flash(flash),_sd(sd),_external(external) {}
    bool run() {
        // Finish all directory routing before filename/read migration, so a
        // failed source removal cannot race a later rewrite of its target.
        for (unsigned phase=0;phase<2;++phase) for (unsigned which=0;which<2;++which) {
            auto store=medium(which); if (!store.ready()) continue;
            char after[40]={}; Names peers;
            for (size_t count;(count=next(store,root(which),after,peers,true));) {
                memcpy(after,peers[count-1],sizeof(after));
                for (size_t i=0;i<count;++i) {
                    uint8_t decoded[16]; const size_t length=strlen(peers[i]);
                    if ((length!=16 && length!=32) || !decodeHex(peers[i],length,decoded,length/2)) continue;
                    if (phase==0 && length==16) truncated(which,peers[i]);
                    if (phase==1 && length==32 && LEGACY_MSG_FILENAME_MIGRATION) filenames(which,peers[i]);
                }
            }
            if (_scanFailed) return false;
        }
        if (_skipped>16) Serial.printf("[MSGSTORE] %u legacy items not imported\n",_skipped);
        return true;
    }
    // Recovery scans see only routed names, so the counter must clear every
    // imported or retried one even when this boot stops before that scan.
    void persistFloor() const {
        if (!_floor) return;
        const uint32_t next=_floor==UINT32_MAX ? UINT32_MAX : _floor+1;
        Preferences preferences;
        if (!preferences.begin(NVS_NS_MSG,false)) return;
        if (preferences.getUInt("msgctr",0)<next) preferences.putUInt("msgctr",next);
        preferences.end();
    }
    // Best effort: a marker that cannot be applied stays for a later boot.
    void foldReadMarkers(MessageTransactions& executor) {
        if (!LEGACY_READ_CTR_MIGRATION) return;
        char after[33]={},peer[33]; Error scanError=Error::None;
        while (executor.nextPeer(after,peer,scanError)) {
            memcpy(after,peer,sizeof(after)); uint32_t cutoff=0; bool any=false,readable=true;
            for (unsigned which=0;which<2 && readable;++which) {
                auto store=medium(which); if (!store.ready()) continue;
                char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),peer);
                uint32_t current=0; bool found=false;
                readable=marker(store,directory,".read_ctr",false,current,found);
                cutoff=std::max(cutoff,current); any=any||found;
            }
            if (!readable || !any) continue;
            if (cutoff) {
                // Reuse the semantic highest-revision, backup recovery and
                // optional-mirror writer, but only over existing copies: a
                // card-only history must not be copied into internal flash.
                Request request; request.operation=Operation::MarkRead; request.key.counter=cutoff; request.existingOnly=true;
                if (!decodeHex(peer,32,request.key.peer,16)) continue;
                Result result; executor.execute(request,nullptr,0,0,result);
                if (result.outcome!=Outcome::Committed || result.error!=Error::None) continue;
            }
            for (unsigned which=0;which<2;++which) {
                auto store=medium(which); if (!store.ready()) continue;
                char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),peer);
                removeMarker(store,directory,".read_ctr");
            }
        }
    }

};

} // namespace handheld::storage
