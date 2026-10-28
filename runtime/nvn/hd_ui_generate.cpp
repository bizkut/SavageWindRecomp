// Generate the player's own HD UI pack while shader precompilation uses the CPU.
#include "hd_ui_generate.h"
#include "hd_ui_dds.h"
#include "hd_ui_extract.h"
#include "hd_ui_ncnn.h"
#include "hd_ui_policy.h"
#include "hd_ui_pack_source.h"
#include "../game_sites.h"
#include "../paths.h"
#include "../platform.h"
extern "C" {
#include "../storage_ps5.h"
#include <mbedtls/sha256.h>
}
#include <json/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <pthread.h>
#ifdef __PROSPERO__
#include <sched.h>
#endif

using json = nlohmann::json;
static constexpr int generator_version = HD_UI_GENERATOR_VERSION;
static constexpr size_t pending_limit = 128u << 20;

struct Record {
    uint64_t hash;
    uint32_t format, w, h, channels, scale;
    std::string archive, name, source_digest;
};
struct Completed { std::string digest; size_t bytes; };
struct Pending { uint64_t hash; uint32_t w, h, channels, format; std::vector<uint8_t> pixels; };
struct Work {
    std::mutex lock;
    std::condition_variable cv;
    std::vector<Record> records;
    std::map<uint64_t, Record> unique;
    std::map<uint64_t, Completed> completed;
    std::map<uint64_t, json> previous;
    std::set<uint64_t> queued;
    std::deque<Pending> pending;
    size_t pending_bytes = 0;
    std::atomic<int> done{0}, upscaled{0}, total{0};
    std::atomic<bool> active{true};
    bool shader_reads_done=false, shader_done=false, producer_done=false, failed=false;
    pthread_t producer{}, encoder{};
    bool producer_started=false, encoder_started=false;
    HdUiNcnn *upscaler=nullptr;
    uint64_t start=0;
};
static std::atomic<Work *> work{nullptr};
static std::atomic<bool> loading{false};

static void background_thread(const char *name) {
#ifdef __PROSPERO__
    plat_thread_heap_own();
    struct sched_param p = {};
    p.sched_priority = sched_get_priority_min(SCHED_OTHER);
    int rc = p.sched_priority < 0 ? -1 : pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);
    fprintf(stderr, "[hd-ui-gen] %s low-priority=%d rc=%d\n", name, p.sched_priority, rc);
#else
    (void)name;
#endif
}

static bool wait_for_game_load(Work *w) {
    std::unique_lock<std::mutex> lock(w->lock);
    while (loading.load(std::memory_order_relaxed) && !w->failed)
        w->cv.wait_for(lock, std::chrono::milliseconds(100));
    return !w->failed;
}

static std::string digest(const void *data, size_t size) {
    unsigned char out[32];
    if (mbedtls_sha256_ret((const unsigned char *)data,size,out,0)) return {};
    static const char hex[]="0123456789abcdef";
    std::string s(64,'0');
    for (size_t i=0;i<32;i++) { s[2*i]=hex[out[i]>>4]; s[2*i+1]=hex[out[i]&15]; }
    return s;
}
static std::string hex64(uint64_t v) { char out[17]; snprintf(out,sizeof out,"%016llx",(unsigned long long)v); return out; }
static std::string hexformat(uint32_t v) { char out[9]; snprintf(out,sizeof out,"%04x",v); return out; }
static bool read_data(const char *path, std::vector<uint8_t>& out) {
    FILE *f=rt_data_fopen(path,"rb");
    if (!f) return false;
    bool ok=!fseek(f,0,SEEK_END); long n=ok?ftell(f):-1;
    ok=ok && n>=0 && n<=(long)(1u<<30) && !fseek(f,0,SEEK_SET);
    if (ok) { out.resize((size_t)n); ok=fread(out.data(),1,out.size(),f)==out.size(); }
    fclose(f); return ok;
}
static json load_manifest() {
    std::vector<uint8_t> bytes;
    if (!read_data("hd-ui/manifest.json",bytes)) return json::object();
    return json::parse(bytes.begin(),bytes.end(),nullptr,false);
}
bool hd_ui_generate_needed(void) {
    try {
        json m=load_manifest();
        return !m.is_object() || m.value("version",0)!=1 || m.value("generator_version",0)!=generator_version ||
               m.value("game_version",std::string())!=g_game_version || !m.value("complete",false);
    } catch (...) { return true; }
}
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static bool reusable(const Record& r, const json& old, Completed& result) {
    try {
        if (old.value("source_format",std::string())!=hexformat(r.format) || old.value("width",0u)!=r.w || old.value("height",0u)!=r.h) return false;
        std::string name="hd-ui/"+hex64(r.hash)+".dds";
        std::vector<uint8_t> b;
        if (!read_data(name.c_str(),b) || b.size()<148 || memcmp(b.data(),"DDS ",4) || u32(b.data()+4)!=124 ||
            memcmp(b.data()+84,"DX10",4) || u32(b.data()+12)!=r.h*r.scale || u32(b.data()+16)!=r.w*r.scale) return false;
        uint32_t dxgi=r.channels==1?80:r.channels==2?83:(r.format==0x0b06 || r.format==0x1a06 || r.format==0x1c06)?99:98;
        if (u32(b.data()+128)!=dxgi || u32(b.data()+132)!=3 || u32(b.data()+140)!=1) return false;
        uint32_t w=r.w*r.scale,h=r.h*r.scale,levels=0; size_t size=148;
        for (;;) { size+=(size_t)((w+3)/4)*((h+3)/4)*(r.channels==1?8:16); levels++; if(w==1&&h==1)break; w=w>1?w/2:1; h=h>1?h/2:1; }
        if (b.size()!=size || u32(b.data()+28)!=levels) return false;
        std::string sha=digest(b.data(),b.size());
        if (sha.empty() || old.value("sha256",std::string())!=sha) return false;
        result={sha,b.size()}; return true;
    } catch (...) { return false; }
}
static int publish(Work *w, bool complete) {
    json m={{"version",1},{"generator_version",generator_version},{"game_version",g_game_version},{"complete",complete},{"textures",json::array()}};
    for (const Record& r : w->records) {
        auto it=w->completed.find(r.hash);
        if (it==w->completed.end()) continue;
        m["textures"].push_back({{"hash",hex64(r.hash)},{"archive",r.archive},{"name",r.name},
            {"source_format",hexformat(r.format)},{"width",r.w},{"height",r.h},
            {"output_width",r.w*r.scale},{"output_height",r.h*r.scale},
            {"format",r.channels==1?"BC4":r.channels==2?"BC5":"BC7"},
            {"sha256",it->second.digest},{"bytes",it->second.bytes}});
    }
    std::string data=m.dump(2)+"\n";
    return rt_storage_replace("hd-ui/manifest.json",data.data(),data.size());
}
struct Layout { Work *work; std::string archive; bool generate; };
static int texture(void *context, const HdUiTexture *t) {
    Layout *l=(Layout *)context; Work *w=l->work;
    if (!wait_for_game_load(w)) return -1;
    if (!l->generate) {
        Record r{t->hash,t->format,t->width,t->height,t->channels,t->scale,l->archive,
                 std::string(t->name,t->name_length),digest(t->linear,t->linear_size)};
        if (r.source_digest.empty()) return -1;
        auto old=w->unique.find(r.hash);
        if (old!=w->unique.end() && (old->second.format!=r.format || old->second.w!=r.w || old->second.h!=r.h || old->second.source_digest!=r.source_digest)) {
            fprintf(stderr,"[hd-ui-gen] source hash collision: %s\n",r.name.c_str()); return -1;
        }
        if (old==w->unique.end()) w->unique.emplace(r.hash,r);
        w->records.push_back(std::move(r)); return 0;
    }
    {
        std::lock_guard<std::mutex> lock(w->lock);
        if (w->completed.count(t->hash) || w->queued.count(t->hash)) return 0;
        w->queued.insert(t->hash);
    }
    auto it=w->unique.find(t->hash);
    if (it==w->unique.end() || it->second.source_digest!=digest(t->linear,t->linear_size)) return -1;
    uint64_t t0=plat_now_ns();
    size_t count=(size_t)t->width*t->height, out_count=count*t->scale*t->scale;
    std::vector<uint8_t> input(count*3), rgb(out_count*3);
    Pending item{t->hash,t->width*t->scale,t->height*t->scale,t->channels,t->format,{}};
    item.pixels.resize(out_count*t->channels);
    unsigned passes=t->channels==4?2:t->channels;
    for (unsigned pass=0;pass<passes;pass++) {
        for (size_t p=0;p<count;p++) {
            if (t->channels==4 && pass==0) memcpy(input.data()+3*p,t->pixels+4*p,3);
            else { uint8_t v=t->pixels[p*t->channels+(t->channels==4?3:pass)]; input[3*p]=input[3*p+1]=input[3*p+2]=v; }
        }
        if (hd_ui_ncnn_upscale(w->upscaler,input.data(),t->width,t->height,t->scale,rgb.data())) return -1;
        for (size_t p=0;p<out_count;p++) {
            if (t->channels==4 && pass==0) memcpy(item.pixels.data()+4*p,rgb.data()+3*p,3);
            else {
                unsigned gray=(19595u*rgb[3*p]+38470u*rgb[3*p+1]+7471u*rgb[3*p+2]+32768u)>>16;
                item.pixels[p*t->channels+(t->channels==4?3:pass)]=(uint8_t)gray;
            }
        }
    }
    fprintf(stderr,"[hd-ui-gen] upscale %.*s %ux%u x%u %.1f ms\n",(int)t->name_length,t->name,t->width,t->height,t->scale,(plat_now_ns()-t0)/1e6);
    std::unique_lock<std::mutex> lock(w->lock);
    w->cv.wait(lock,[&]{return w->failed || w->pending_bytes+item.pixels.size()<=pending_limit || w->pending.empty();});
    if (w->failed) return -1;
    w->pending_bytes+=item.pixels.size(); w->pending.push_back(std::move(item)); w->cv.notify_all();
    ++w->upscaled;
    return 0;
}
static int layout(void *context, const char *pack, const char *name, size_t name_len, const uint8_t *data, size_t size) {
    if (!wait_for_game_load((Work *)context)) return -1;
    Layout l{(Work *)context,std::string(pack)+":"+std::string(name,name_len),false};
    return hd_ui_extract_layout(data,size,texture,&l);
}
static int layout_generate(void *context, const char *pack, const char *name, size_t name_len, const uint8_t *data, size_t size) {
    if (!wait_for_game_load((Work *)context)) return -1;
    Layout l{(Work *)context,std::string(pack)+":"+std::string(name,name_len),true};
    return hd_ui_extract_layout(data,size,texture,&l);
}
static void *encode_main(void *arg) {
    Work *w=(Work *)arg;
    background_thread("encoder");
    try {
        for (;;) {
            std::unique_lock<std::mutex> lock(w->lock);
            while (!w->failed && (!w->shader_done || loading.load(std::memory_order_relaxed) ||
                                  (w->pending.empty() && !w->producer_done)))
                w->cv.wait_for(lock, std::chrono::milliseconds(100));
            if (w->failed || (w->pending.empty() && w->producer_done)) break;
            Pending item=std::move(w->pending.front()); w->pending.pop_front();
            w->pending_bytes-=item.pixels.size(); w->cv.notify_all(); lock.unlock();
            uint8_t *dds=nullptr; size_t bytes=0;
            int srgb=item.format==0x0b06 || item.format==0x1a06 || item.format==0x1c06;
            if (hd_ui_dds_encode(item.pixels.data(),item.w,item.h,item.channels,srgb,&dds,&bytes)) throw std::runtime_error("DDS encode failed");
            std::string sha=digest(dds,bytes), path="hd-ui/"+hex64(item.hash)+".dds";
            int r=sha.empty()?-1:rt_storage_replace(path.c_str(),dds,bytes); free(dds);
            if (r<0) throw std::runtime_error("DDS storage failed");
            lock.lock(); w->completed[item.hash]={sha,bytes};
            if (publish(w,false)<0) throw std::runtime_error("manifest storage failed");
            int done=++w->done; fprintf(stderr,"[hd-ui-gen] HD interface %d of %d\n",done,w->total.load());
        }
    } catch (...) { std::lock_guard<std::mutex> lock(w->lock); w->failed=true; w->cv.notify_all(); }
    return nullptr;
}
static void *producer_main(void *arg) {
    Work *w=(Work *)arg;
    background_thread("producer");
    try {
        w->start=plat_now_ns();
        fprintf(stderr,"[hd-ui-gen] start game=%s generator=%d\n",g_game_version,generator_version);
        {
            std::unique_lock<std::mutex> lock(w->lock);
            w->cv.wait(lock, [&]{ return w->shader_reads_done || w->failed; });
            if (w->failed) throw std::runtime_error("generation stopped");
        }
        fprintf(stderr,"[hd-ui-gen] shader archive reads complete; scanning layouts\n");
        if (rt_storage_mkdirs("hd-ui")<0) throw std::runtime_error("mkdir failed");
        json old=load_manifest();
        if (old.is_object() && old.value("version",0)==1 &&
            (!old.contains("generator_version") || old.value("generator_version",0)==generator_version) &&
            (!old.contains("game_version") || old.value("game_version",std::string())==g_game_version) && old.contains("textures") && old["textures"].is_array()) {
            for (const auto &r:old["textures"]) {
                std::string hash=r.value("hash",std::string()); char *end=nullptr;
                if (hash.size()==16) { uint64_t h=strtoull(hash.c_str(),&end,16); if (end && !*end) w->previous[h]=r; }
            }
        }
        if (fs_hd_ui_layouts(layout,w)) throw std::runtime_error("layout enumeration failed");
        w->total=(int)w->unique.size();
        for (const auto& pair:w->unique) {
            auto p=w->previous.find(pair.first); Completed c;
            if (p!=w->previous.end() && reusable(pair.second,p->second,c)) w->completed[pair.first]=std::move(c);
        }
        w->done=(int)w->completed.size();
        w->upscaled=w->done.load();
        if (publish(w,false)<0) throw std::runtime_error("manifest storage failed");
        if (w->completed.size()!=w->unique.size()) {
            char p2[1024],p4[1024],weights[1024];
            if (!rt_resource("realesr-animevideov3-x2.param",nullptr,p2,sizeof p2) ||
                !rt_resource("realesr-animevideov3-x4.param",nullptr,p4,sizeof p4) ||
                !rt_resource("realesr-animevideov3-x4.bin",nullptr,weights,sizeof weights)) throw std::runtime_error("model missing");
            w->upscaler=hd_ui_ncnn_create(p2,p4,weights);
            if (!w->upscaler) throw std::runtime_error("ncnn initialization failed");
            if (pthread_create(&w->encoder,nullptr,encode_main,w)) throw std::runtime_error("encoder thread failed");
            w->encoder_started=true;
            if (fs_hd_ui_layouts(layout_generate,w)) throw std::runtime_error("upscaling failed");
        }
        { std::lock_guard<std::mutex> lock(w->lock); w->producer_done=true; w->cv.notify_all(); }
        if (w->encoder_started) { pthread_join(w->encoder,nullptr); w->encoder_started=false; }
        if (w->failed || w->completed.size()!=w->unique.size()) throw std::runtime_error("generation incomplete");
        if (publish(w,true)<0) throw std::runtime_error("manifest finalization failed");
        fprintf(stderr,"[hd-ui-gen] complete %d textures in %.1f s\n",w->done.load(),(plat_now_ns()-w->start)/1e9);
    } catch (const std::exception& e) {
        fprintf(stderr,"[hd-ui-gen] failed: %s\n",e.what());
        { std::lock_guard<std::mutex> lock(w->lock); w->failed=true; w->producer_done=true; w->cv.notify_all(); }
        if (w->encoder_started) pthread_join(w->encoder,nullptr);
    } catch (...) {
        fprintf(stderr,"[hd-ui-gen] failed: unknown error\n");
        { std::lock_guard<std::mutex> lock(w->lock); w->failed=true; w->producer_done=true; w->cv.notify_all(); }
        if (w->encoder_started) pthread_join(w->encoder,nullptr);
    }
    hd_ui_ncnn_destroy(w->upscaler); w->active=false; return nullptr;
}
bool hd_ui_generate_start(void) {
    if (work.load()) return false;
    Work *w=new (std::nothrow) Work;
    if (!w) return false;
    work.store(w);
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,8u<<20);
    int r=pthread_create(&w->producer,&attr,producer_main,w); pthread_attr_destroy(&attr);
    if (r) { w->active=false; return false; }
    w->producer_started=true;
    return true;
}
void hd_ui_generate_shader_reads_done(void) {
    Work *w=work.load();
    if (!w) return;
    std::lock_guard<std::mutex> lock(w->lock); w->shader_reads_done=true; w->cv.notify_all();
}
void hd_ui_generate_shaders_done(void) {
    Work *w=work.load();
    if (!w) return;
    std::lock_guard<std::mutex> lock(w->lock);
    w->shader_reads_done=true; w->shader_done=true; w->cv.notify_all();
}
void hd_ui_generate_set_loading(bool value) {
    bool was = loading.exchange(value, std::memory_order_relaxed);
    if (was != value && work.load())
        fprintf(stderr, "[hd-ui-gen] %s for loading screen\n", value ? "pausing" : "resuming");
}
bool hd_ui_generate_progress(int *done,int *total) {
    Work *w=work.load();
    if (!w) return false;
    if (done) { std::lock_guard<std::mutex> lock(w->lock); *done=w->shader_done ? w->done.load() : w->upscaled.load(); }
    if (total) *total=w->total;
    return w->active;
}
void hd_ui_generate_join(void) {
    Work *w=work.exchange(nullptr);
    if (!w) return;
    if (w->producer_started) pthread_join(w->producer,nullptr);
    delete w;
}
