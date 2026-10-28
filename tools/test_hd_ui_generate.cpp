// Host integration test: real inference and encoding, file-backed stand-ins for the PS5 broker.
#include "../runtime/nvn/hd_ui_generate.h"
#include "../runtime/nvn/hd_ui_pack_source.h"
#include "../runtime/paths.h"
#include <json/json.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static std::string root, model, layout;
static bool deny_model;
extern "C" const char *g_game_version = "1.9.0";
extern "C" const char *rt_data_root(void) { return root.c_str(); }
extern "C" bool rt_resource(const char *name, const char *, char *out, size_t n) {
    if (deny_model) return false;
    std::string p=model+"/"+name;
    if (!std::filesystem::exists(p) || p.size()>=n) return false;
    memcpy(out,p.c_str(),p.size()+1); return true;
}
extern "C" uint64_t plat_now_ns(void) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" int rt_storage_mkdirs(const char *p) { return std::filesystem::create_directories(root+"/"+p) || std::filesystem::is_directory(root+"/"+p) ? 0 : -1; }
extern "C" int rt_storage_replace(const char *p, const void *data, size_t size) {
    std::string path=root+"/"+p, temp=path+".tmp";
    FILE *f=fopen(temp.c_str(),"wb"); if (!f) return -1;
    bool ok=fwrite(data,1,size,f)==size; ok=fclose(f)==0 && ok;
    return ok && rename(temp.c_str(),path.c_str())==0 ? 0 : -1;
}
extern "C" int fs_hd_ui_layouts(HdUiLayoutCallback callback, void *context) {
    std::ifstream f(layout,std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
    if (!f.eof() && f.fail()) return -1;
    const char *name="Layout/Common.sblarc";
    return callback(context,"Pack/Bootup.pack",name,strlen(name),bytes.data(),bytes.size());
}
static void wait_finished() {
    for (int i=0;i<6000;i++) {
        if (!hd_ui_generate_progress(nullptr,nullptr)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!"generation timeout");
}
int main(int argc,char **argv) {
    assert(argc==4); root=argv[1]; model=argv[2]; layout=argv[3];
    assert(hd_ui_generate_needed());
    assert(hd_ui_generate_start());
    int done=0,total=0;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(hd_ui_generate_progress(&done,&total) && total==0 && done==0);
    hd_ui_generate_shader_reads_done();
    for (int i=0;i<6000;i++) {
        assert(hd_ui_generate_progress(&done,&total));
        if (total==1 && done==1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(total==1 && done==1);
    // CPU compression is intentionally held until shader workers have finished.
    for (const auto& entry:std::filesystem::directory_iterator(root+"/hd-ui")) assert(entry.path().extension() != ".dds");
    hd_ui_generate_set_loading(true);
    hd_ui_generate_shaders_done();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(hd_ui_generate_progress(nullptr,nullptr));
    for (const auto& entry:std::filesystem::directory_iterator(root+"/hd-ui")) assert(entry.path().extension() != ".dds");
    hd_ui_generate_set_loading(false);
    wait_finished(); hd_ui_generate_join();
    assert(!hd_ui_generate_needed());
    std::ifstream in(root+"/hd-ui/manifest.json"); nlohmann::json manifest; in>>manifest;
    assert(manifest.at("complete")==true && manifest.at("textures").size()==1);
    std::string dds=root+"/hd-ui/"+manifest["textures"][0]["hash"].get<std::string>()+".dds";
    assert(std::filesystem::exists(dds)); auto before=std::filesystem::last_write_time(dds);
    int version=manifest.at("generator_version").get<int>();
    manifest["generator_version"]=version-1; { std::ofstream out(root+"/hd-ui/manifest.json"); out<<manifest; }
    assert(hd_ui_generate_needed());
    manifest["generator_version"]=version;
    manifest["complete"]=false; { std::ofstream out(root+"/hd-ui/manifest.json"); out<<manifest; }
    deny_model=true; assert(hd_ui_generate_needed()); assert(hd_ui_generate_start());
    hd_ui_generate_shaders_done(); wait_finished(); hd_ui_generate_join();
    assert(!hd_ui_generate_needed() && std::filesystem::last_write_time(dds)==before);
    g_game_version="2.0"; assert(hd_ui_generate_needed());
    puts("HD UI scheduling and manifest resume passed");
}
