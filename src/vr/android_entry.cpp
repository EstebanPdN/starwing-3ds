#include "starfox/vr/application.hpp"
#include "starfox/assets/embedded.hpp"
#include "starfox/assets/runtime_bundle.hpp"
#include <fstream>
#include <jni.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <android/log.h>
#include <iostream>
#include <streambuf>

namespace {
// Android discards ordinary process stderr. Preserve the shared application's
// diagnostics in app-scoped logcat instead of losing rendering failure details.
class AndroidErrors final : public std::streambuf {
    std::string line_;
    std::ostream& stream_;
    int priority_;
    std::streambuf* previous_;
    void flush_line() {
        if (!line_.empty()) {
            __android_log_write(priority_, "StarFoxVR", line_.c_str());
            line_.clear();
        }
    }
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            const char c = traits_type::to_char_type(value);
            if (c == '\n') flush_line();
            else {line_ += c;if (line_.size() >= 1024) flush_line();}
        }
        return traits_type::not_eof(value);
    }
    int sync() override {flush_line();return 0;}
public:
    explicit AndroidErrors(std::ostream& stream=std::cerr,int priority=ANDROID_LOG_ERROR)
        :stream_(stream),priority_(priority),previous_(stream.rdbuf(this)) {}
    ~AndroidErrors() override {stream_.rdbuf(previous_);flush_line();}
};
class GlobalRef {
    JNIEnv* env_;
    jobject value_;
public:
    GlobalRef(JNIEnv* env, jobject value):env_(env),value_(value?env->NewGlobalRef(value):nullptr) {
        if(!value_) throw std::runtime_error("Missing Quest host reference");
    }
    ~GlobalRef(){env_->DeleteGlobalRef(value_);}
    GlobalRef(const GlobalRef&)=delete;
    jobject get() const {return value_;}
};
std::string path(JNIEnv* env,jstring value) {
    if(!value) throw std::runtime_error("Missing Quest asset path");
    const char* chars=env->GetStringUTFChars(value,nullptr);
    if(!chars) throw std::runtime_error("Cannot read Quest asset path");
    std::string result;
    try {result=chars;} catch(...) {env->ReleaseStringUTFChars(value,chars);throw;}
    env->ReleaseStringUTFChars(value,chars);
    if(result.empty()) throw std::runtime_error("Empty Quest asset path");
    return result;
}
}

// Invoked on a Java-owned worker thread, never on the Activity's UI thread.
extern "C" JNIEXPORT void JNICALL
Java_com_starfox_enhanced_quest_QuestBridge_validateBundle(JNIEnv* env,jclass,jstring file) {
    try {
        std::ifstream input(path(env,file),std::ios::binary|std::ios::ate);
        if(!input) throw std::runtime_error("Cannot open imported asset bundle");
        const auto size=input.tellg();
        if(size<=0 || size>64*1024*1024) throw std::runtime_error("Invalid asset bundle size");
        std::vector<std::uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
        if(!input.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))
            throw std::runtime_error("Incomplete asset bundle");
        (void)starfox::assets::decode_runtime_bundle(bytes,
            starfox::assets::runtime_companion_manifest(starfox::assets::embedded_asset));
    } catch(const std::exception& error) {
        if(!env->ExceptionCheck()) {
            const auto type=env->FindClass("java/lang/IllegalStateException");
            if(type) {env->ThrowNew(type,error.what());env->DeleteLocalRef(type);}
        }
    }
}
// All JNI access uses that thread's JNIEnv; no native handle outlives this call.
extern "C" JNIEXPORT jint JNICALL
Java_com_starfox_enhanced_quest_QuestBridge_run(JNIEnv* env,jclass,jobject activity,
        jobject context,jstring rom,jstring symbols,jobject stop) {
    AndroidErrors diagnostics;
    AndroidErrors progress(std::cout,ANDROID_LOG_INFO);
    try {
        JavaVM* vm=nullptr;
        if(env->GetJavaVM(&vm)!=JNI_OK) throw std::runtime_error("Cannot obtain Java VM");
        GlobalRef activity_ref(env,activity),context_ref(env,context),stop_ref(env,stop);
        jclass stop_class=env->GetObjectClass(stop_ref.get());
        if(!stop_class) throw std::runtime_error("Missing cancellation class");
        const auto get=env->GetMethodID(stop_class,"get","()Z");
        env->DeleteLocalRef(stop_class);
        if(!get) throw std::runtime_error("Missing cancellation accessor");
        starfox::vr::AndroidXrContext android{vm,context_ref.get(),activity_ref.get()};
        starfox::vr::ApplicationHost host;
        host.android=&android;host.frame_limit=0;host.time_limit=std::chrono::seconds(0);
        host.stop_requested=[&] {
            const bool requested=env->CallBooleanMethod(stop_ref.get(),get)==JNI_TRUE;
            if(env->ExceptionCheck()) throw std::runtime_error("Quest cancellation callback failed");
            return requested;
        };
        std::vector<std::string> arguments{"starfox_quest",symbols?"--intro":"--bundle",path(env,rom)};
        if(symbols) arguments.push_back(path(env,symbols));
        host.cartridge_save_path=std::filesystem::path(arguments[2]).parent_path()/"starfox-ex.srm";
        std::vector<char*> argv;
        for(auto& argument:arguments) argv.push_back(argument.data());
        return starfox::vr::run_application(static_cast<int>(argv.size()),argv.data(),host);
    } catch(const std::exception& error) {
        if(!env->ExceptionCheck()) {
            const auto type=env->FindClass("java/lang/IllegalStateException");
            if(type) {env->ThrowNew(type,error.what());env->DeleteLocalRef(type);}
        }
    } catch(...) {
        if(!env->ExceptionCheck()) {
            const auto type=env->FindClass("java/lang/IllegalStateException");
            if(type) {env->ThrowNew(type,"Unexpected Quest native failure");env->DeleteLocalRef(type);}
        }
    }
    return -1;
}
#include "starfox/assets/embedded.hpp"
#include "starfox/assets/runtime_bundle.hpp"
#include <fstream>
