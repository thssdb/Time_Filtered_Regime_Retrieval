#include "aligned_pages.hpp"
#include <jni.h>
using namespace nanhang_weighted_fetch;
namespace {
std::vector<std::unique_ptr<Mapping>> mappings;
std::vector<Block> blocks;
std::vector<Byte> scratch;
}
extern "C" JNIEXPORT void JNICALL Java_GroupRice_openTime(JNIEnv* env,jclass,jstring value) {
    const char* raw=env->GetStringUTFChars(value,nullptr);
    if (!raw) return;
    const fs::path dir(raw);
    env->ReleaseStringUTFChars(value,raw);
    try {
        std::map<int,Block> by_id;
        for(const auto& file:files_with_suffix(dir,".tsfile")){
            mappings.push_back(std::make_unique<Mapping>(file));
            index_file(*mappings.back(),by_id);
        }
        for(auto& [id,block]:by_id){
            block.count=ids_from(block.ids,scratch).size();
            if(block.template_groups.empty() && !block.plain_time && block.time.codec==0) index_time_restarts(block);
            blocks.push_back(std::move(block));
        }
    } catch (const std::exception& error) {
        env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    }
}
extern "C" JNIEXPORT void JNICALL Java_GroupRice_readTime(JNIEnv* env,jclass,jintArray locations,jlongArray timestamps){
    std::array<jint,10> address;
    std::array<int64_t,180> values;
    const int count=env->GetArrayLength(locations);
    if(count>10 || env->GetArrayLength(timestamps)!=count*18) {
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"unexpected result size");
        return;
    }
    env->GetIntArrayRegion(locations,0,count,address.data());
    try {
        for(int i=0;i<count;i++) read_times(blocks.at(unsigned(address[i])>>14),(address[i]>>7)&127,values.data()+18*i,scratch);
    } catch (const std::exception& error) {
        env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
        return;
    }
    static_assert(sizeof(jlong)==sizeof(int64_t));
    env->SetLongArrayRegion(timestamps,0,count*18,reinterpret_cast<const jlong*>(values.data()));
}
