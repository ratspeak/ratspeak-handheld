#pragma once

#include "MessageRecord.h"
#include "MessageTransactions.h"
#include "FlashStore.h"
#include "SDStore.h"
#include "config/Config.h"

namespace handheld::storage {

// Boot-only, under the filesystem lease and before queue admission. Reopening
// directories for each lexical successor keeps memory independent of history.
// Legacy names retain their numeric counter: mirrored donor bytes then resolve
// to the same key, and .read_ctr keeps its original ordering meaning.
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
    FlashStore* _flash;
    SDStore* _sd;
    bool _external;
    bool _scanFailed = false;
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
    // The caller stores one bounded successor, never a list of directories/files.
    bool next(Medium store, const char* directory, const char* after, char (&out)[40], bool dirs) {
        out[0]=0; File dir=store.open(directory);
        if (!dir || !dir.isDirectory()) { if (store.exists(directory)) _scanFailed = true; return false; }
        for (File entry=dir.openNextFile();entry;entry=dir.openNextFile()) {
            const char* name=basename(entry.name()); const size_t length=strnlen(name,sizeof(out));
            if (!dirs && !entry.isDirectory() && length>=sizeof(out) && messageLike(name)) { _scanFailed=true; return false; }
            if (entry.isDirectory()!=dirs || length>=sizeof(out) || strcmp(name,after)<=0 ||
                (out[0] && strcmp(name,out)>=0)) continue;
            memcpy(out,name,length+1);
        }
        return out[0]!=0;
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
        if (!header(store,path,document,record)) return false;
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
    // Unequal revisions may differ in the three mutable storage fields. Compare
    // every other known field, including exact timestamp values,
    // with one parser arena and 512 bytes of fixed slice scratch. This slower fallback is
    // only used when ordinary byte comparison differs; no second body/document
    // or probabilistic hash is used to authorize source removal.
    static bool sameContent(Medium sourceStore,const char* sourcePath,Medium targetStore,const char* targetPath) {
        MessageDocument document; uint32_t sourceRevision=0,targetRevision=0;size_t sourceLength=0,targetLength=0;
        double sourceTime=0,targetTime=0;
        if (!normalized(sourceStore,sourcePath,document,sourceRevision,sourceLength,&sourceTime) ||
            !normalized(targetStore,targetPath,document,targetRevision,targetLength,&targetTime) ||
            sourceRevision==targetRevision || sourceLength!=targetLength || sourceTime!=targetTime) return false;
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
    // Never overwrite a surviving target or its rollback copy. A cut after
    // verified copy but before removal retries by exact comparison at this key.
    static bool move(Medium store, const char* source, const char* target) {
        if (!strcmp(source,target)) return true;
        if (!store.exists(target)) {
            char backup[128]; const int n=snprintf(backup,sizeof(backup),"%s.bak",target);
            if (n<0 || n>=int(sizeof(backup))) return false;
            if (store.exists(backup)) {
                if (!equal(store,source,backup) || !store.rename(backup,target)) return false;
            } else {
                File file=store.open(source); if (!file || !file.size() || file.size()>Budget::MaxStoredFile) return false;
                FileSource bytes(file); if (!store.write(target,bytes)) return false;
            }
        }
        return equal(store,source,target) && store.remove(source);
    }
    static bool marker(Medium store, const char* directory, const char* name, bool json,
                       uint32_t& value, bool& found, bool* backupBest = nullptr) {
        value=0; found=false;
        if (backupBest) *backupBest=false;
        bool primaryFound=false;
        for (const char* suffix : {"",".bak"}) {
            char path[128]; const int n=snprintf(path,sizeof(path),"%s/%s%s",directory,name,suffix);
            if (n<0 || n>=int(sizeof(path))) return false;
            if (!store.exists(path)) continue;
            found=true; File file=store.open(path); if (!file) return false;
            uint32_t current=0;
            if (json) {
                MessageDocument document;
                if (file.size()>512 || document.parse(file)!=Error::None ||
                    !document.document()["through"].is<uint32_t>()) return false;
                current=document.document()["through"].as<uint32_t>();
            } else {
                char bytes[32]; const size_t size=file.size();
                if (!size || size>=sizeof(bytes) || file.read(reinterpret_cast<uint8_t*>(bytes),size)!=size) return false;
                size_t end=size; while (end && (bytes[end-1]=='\n' || bytes[end-1]=='\r' || bytes[end-1]==' ')) --end;
                if (!decimal(bytes,end,current)) return false;
            }
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
        uint32_t current=0; bool found=false,backupBest=false;
        if (!marker(store,directory,name,json,current,found,&backupBest)) return false;
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
    static bool header(Medium store,const char* path,MessageDocument& document,StoredRecordHeader& out) {
        File file=store.open(path); out={};
        return file && document.parse(file)==Error::None && recordHeader(document.document(),out);
    }
    bool truncated(unsigned which,const char* shortPeer) {
        auto store=medium(which); char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),shortPeer);
        uint32_t readThrough=0,deleteThrough=0; bool haveRead=false,haveDelete=false;
        if (!marker(store,directory,".read_ctr",false,readThrough,haveRead) ||
            !marker(store,directory,".deleted",true,deleteThrough,haveDelete)) return false;
        bool sawRecord=false; char after[40]={},name[40];
        while (next(store,directory,after,name,false)) {
            memcpy(after,name,sizeof(after)); if (!messageLike(name)) continue;
            Name parsed; if (!filename(name,parsed) || (!parsed.canonical && !LEGACY_MSG_FILENAME_MIGRATION)) return false;
            char source[128]; if (!join(directory,name,source)) return false;
            MessageDocument document; StoredRecordHeader record;
            if (!header(store,source,document,record) || record.incoming!=parsed.incoming) return false;
            char fullPeer[33]; encodeHex(parsed.incoming ? record.source : record.destination,16,fullPeer);
            if (strncmp(fullPeer,shortPeer,16) || (!parsed.canonical && (haveDelete || deleted(fullPeer)))) return false;
            document.reset(); char targetDir[128],target[128];
            snprintf(targetDir,sizeof(targetDir),"%s/%s",root(which),fullPeer);
            if (!store.mkdir(targetDir) || !join(targetDir,name,target)) return false;
            if (parsed.canonical && !otherCopy(which,fullPeer,name,source)) return false;
            if (haveRead && !mergeMarker(store,targetDir,".read_ctr",false,readThrough)) return false;
            if (haveDelete) {
                char flashDir[128]; snprintf(flashDir,sizeof(flashDir),"%s/%s",PATH_MESSAGES,fullPeer);
                auto flash=medium(0);
                if (!flash.mkdir(flashDir) || !mergeMarker(flash,flashDir,".deleted",true,deleteThrough)) return false;
            }
            // Copy first, but keep the last source until routing metadata has
            // been retired. A power cut cannot leave only an ambiguous marker.
            if (!store.exists(target)) {
                File file=store.open(source);
                if (!file || !file.size() || file.size()>Budget::MaxStoredFile) return false;
                FileSource bytes(file);
                char backup[128]; snprintf(backup,sizeof(backup),"%s.bak",target);
                if (store.exists(backup)) {
                    if (!equal(store,source,backup) || !store.rename(backup,target)) return false;
                } else if (!store.write(target,bytes)) return false;
            }
            if (!equal(store,source,target)) return false;
            char later[40],cursor[40]; memcpy(cursor,name,sizeof(cursor)); bool more=false;
            while (next(store,directory,cursor,later,false)) {
                memcpy(cursor,later,sizeof(cursor)); if (messageLike(later)) { more=true; break; }
            }
            if (_scanFailed) return false;
            if (!more && (!removeMarker(store,directory,".read_ctr") || !removeMarker(store,directory,".deleted"))) return false;
            if (!store.remove(source)) return false;
            sawRecord=true; yield();
        }
        if (_scanFailed || (!sawRecord && (haveRead || haveDelete))) return false;
        store.rmdir(directory); return true; // Empty remnants are harmless.
    }
    bool filenames(unsigned which,const char* peer) {
        auto store=medium(which); char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),peer);
        char after[40]={},name[40];
        while (next(store,directory,after,name,false)) {
            memcpy(after,name,sizeof(after)); if (!messageLike(name)) continue;
            Name parsed; if (!filename(name,parsed)) return false;
            if (parsed.canonical) continue;
            // Earlier donor migration renumbered files. Without a proven old
            // mapping, assigning this late import past a deletion cutoff would
            // resurrect history. Keep the source for explicit recovery instead.
            if (deleted(peer)) return false;
            char source[128]; if (!join(directory,name,source)) return false;
            MessageDocument document; StoredRecordHeader record;
            if (!header(store,source,document,record) || record.incoming!=parsed.incoming) return false;
            char fullPeer[33]; encodeHex(parsed.incoming ? record.source : record.destination,16,fullPeer);
            if (strcmp(peer,fullPeer)) return false;
            document.reset(); char target[128],canonical[32];
            snprintf(canonical,sizeof(canonical),"%013lu_%c.json%s",(unsigned long)parsed.counter,parsed.incoming?'i':'o',parsed.backup?".bak":"");
            if (!otherCopy(which,peer,canonical,source) ||
                !join(directory,canonical,target) || !move(store,source,target)) return false;
            yield();
        }
        return true;
    }
public:
    MessageMigration(FlashStore* flash,SDStore* sd,bool external) : _flash(flash),_sd(sd),_external(external) {}
    bool run() {
        // Finish all directory routing before filename/read migration, so a
        // failed source removal cannot race a later rewrite of its target.
        for (unsigned phase=0;phase<2;++phase) for (unsigned which=0;which<2;++which) {
            auto store=medium(which); if (!store.ready()) continue;
            char after[40]={},peer[40];
            while (next(store,root(which),after,peer,true)) {
                memcpy(after,peer,sizeof(after)); uint8_t decoded[16]; const size_t length=strlen(peer);
                if ((length!=16 && length!=32) || !decodeHex(peer,length,decoded,length/2)) continue;
                if (phase==0 && length==16) { if (!truncated(which,peer)) return false; }
                if (phase==1 && length==32 && LEGACY_MSG_FILENAME_MIGRATION) { if (!filenames(which,peer)) return false; }

            }
        }
        return !_scanFailed;
    }
    bool foldReadMarkers(MessageTransactions& executor) {
        if (!LEGACY_READ_CTR_MIGRATION) return true;
        char after[33]={},peer[33]; Error scanError=Error::None;
        while (executor.nextPeer(after,peer,scanError)) {
            memcpy(after,peer,sizeof(after)); uint32_t cutoff=0; bool any=false;
            for (unsigned which=0;which<2;++which) {
                auto store=medium(which); if (!store.ready()) continue;
                char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),peer);
                uint32_t current=0; bool found=false;
                if (!marker(store,directory,".read_ctr",false,current,found)) return false;
                cutoff=std::max(cutoff,current); any=any||found;
            }
            if (!any) continue;
            if (cutoff) {
                // Reuse the semantic highest-revision, backup recovery and
                // optional-mirror writer. This runs on the boot owner before
                // enabling deferred execution or admitting queue work.
                Request request; request.operation=Operation::MarkRead; request.key.counter=cutoff;
                if (!decodeHex(peer,32,request.key.peer,16)) return false;
                Result result; executor.execute(request,nullptr,0,0,result);
                if (result.outcome!=Outcome::Committed || result.error!=Error::None) return false;
            }
            for (unsigned which=0;which<2;++which) {
                auto store=medium(which); if (!store.ready()) continue;
                char directory[128]; snprintf(directory,sizeof(directory),"%s/%s",root(which),peer);
                if (!removeMarker(store,directory,".read_ctr")) return false;
            }
        }
        return scanError==Error::None;
    }

};

} // namespace handheld::storage
