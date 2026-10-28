// Host test for content-hash lookup and DDS compressed mip validation.
#include "../runtime/nvn/sharp_ui.h"
#include "../runtime/nvn/hd_ui_guard.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char root[] = "/tmp/savagewind-hd-ui-test-XXXXXX";
const char *rt_data_root(void) { return root; }
static void put32(uint8_t *p, uint32_t x) { for (int i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i)); }
int main(void) {
    // First storage/copy/render use requires restoration and permanently blocks another HD image.
    bool blocked=false;
    assert(hd_ui_begin_guest_use(&blocked,true)); assert(blocked);
    assert(!hd_ui_begin_guest_use(&blocked,true)); assert(blocked);
    blocked=false; assert(!hd_ui_begin_guest_use(&blocked,false)); assert(blocked);
    assert(mkdtemp(root)); char dir[256], path[256];
    snprintf(dir,sizeof dir,"%s/hd-ui",root); assert(!mkdir(dir,0700));
    uint8_t original[8]={0}; uint64_t hash=0xcbf29ce484222325ull;
    for (size_t i=0;i<sizeof original;i++) hash=(hash^original[i])*0x100000001b3ull;
    snprintf(path,sizeof path,"%s/%016llx.dds",dir,(unsigned long long)hash);
    uint8_t header[148]={0}; memcpy(header,"DDS ",4); put32(header+4,124); put32(header+12,16);
    put32(header+16,16); put32(header+28,5); put32(header+76,32); memcpy(header+84,"DX10",4);
    put32(header+128,80); put32(header+132,3); put32(header+140,1);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(header,1,sizeof header,f)==sizeof header);
    uint8_t data[184]={0}; assert(fwrite(data,1,sizeof data,f)==sizeof data); assert(!fclose(f));
    uint8_t colour[8]={1}; uint64_t colour_hash=0xcbf29ce484222325ull;
    for (size_t i=0;i<sizeof colour;i++) colour_hash=(colour_hash^colour[i])*0x100000001b3ull;
    char colour_path[256]; snprintf(colour_path,sizeof colour_path,"%s/%016llx.dds",dir,(unsigned long long)colour_hash);
    put32(header+128,99);
    f=fopen(colour_path,"wb"); assert(f); assert(fwrite(header,1,sizeof header,f)==sizeof header);
    assert(fwrite(data,1,sizeof data,f)==sizeof data); assert(fwrite(data,1,sizeof data,f)==sizeof data); assert(!fclose(f));
    uint8_t rg8[32]; const uint8_t pattern[4]={0x12,0x34,0x56,0x78};
    for (size_t i=0;i<sizeof rg8;i++) rg8[i]=pattern[i%4];
    uint64_t rg_hash=0xcbf29ce484222325ull;
    for (size_t i=0;i<sizeof rg8;i++) rg_hash=(rg_hash^rg8[i])*0x100000001b3ull;
    assert(rg_hash==0xf0725719d3338025ull);
    char rg_path[256]; snprintf(rg_path,sizeof rg_path,"%s/%016llx.dds",dir,(unsigned long long)rg_hash);
    put32(header+128,83);
    f=fopen(rg_path,"wb"); assert(f); assert(fwrite(header,1,sizeof header,f)==sizeof header);
    assert(fwrite(data,1,sizeof data,f)==sizeof data); assert(fwrite(data,1,sizeof data,f)==sizeof data); assert(!fclose(f));
    char manifest[256]; snprintf(manifest,sizeof manifest,"%s/manifest.json",dir);
    f=fopen(manifest,"wb"); assert(f);
    fprintf(f,"{\n \"textures\": [\n {\n \"hash\": \"%016llx\",\n \"name\": \"heart\",\n \"source_format\": \"0901\",\n \"width\": 48,\n \"height\": 48,\n \"bytes\": 1\n }\n ]\n}\n",(unsigned long long)colour_hash);
    assert(!fclose(f));
    char diag[256]; snprintf(diag,sizeof diag,"%s/diag.txt",dir);
    f=fopen(diag,"wb"); assert(f); assert(!fclose(f));
    assert(hd_ui_diag_candidate(48,48)); assert(!hd_ui_diag_candidate(48,47));
    hd_ui_diag_upload(48,48,0x39,1,1,0,1,false,false,true,colour,sizeof colour);
    HdUiImage image; assert(hd_ui_load(original,sizeof original,4,4,0x49,&image));
    assert(image.hash==hash && image.width==16 && image.height==16 && image.levels==5 && image.dxgi==80 && image.bytes==184);
    free(image.data);
    assert(hd_ui_load(colour,sizeof colour,4,4,0x38,&image));
    assert(image.hash==colour_hash && image.dxgi==99 && image.bytes==368 && image.levels==5);
    free(image.data);
    assert(hd_ui_load(rg8,sizeof rg8,4,4,0x0d,&image));
    assert(image.hash==rg_hash && image.dxgi==83 && image.bytes==368); free(image.data);
    assert(!hd_ui_load(rg8,sizeof rg8,4,4,0x38,&image));
    assert(!hd_ui_load(original,sizeof original,4,4,0x4b,&image));
    assert(!hd_ui_load(original,sizeof original,8,4,0x49,&image));
    assert(!truncate(colour_path, 148+367));
    assert(!hd_ui_load(colour,sizeof colour,4,4,0x38,&image));
    original[0]=2; assert(!hd_ui_load(original,sizeof original,4,4,0x49,&image));
    assert(!unlink(path)); assert(!unlink(colour_path)); assert(!unlink(rg_path)); assert(!unlink(manifest)); assert(!unlink(diag)); assert(!rmdir(dir)); assert(!rmdir(root));
    puts("HD UI DDS lookup and validation passed"); return 0;
}
