#include "starfox/vr/openxr_runtime.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace {
unsigned creates=0,destroys=0;bool fail_system=false,graphics=true;unsigned eye_count=2;
bool change_eye_count=false;
void check(bool value){if(!value) throw std::runtime_error("OpenXR runtime lifecycle assertion failed");}
}
extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char*,uint32_t capacity,uint32_t* count,XrExtensionProperties* out) {
    *count=graphics?1:0;if(capacity && graphics) std::strcpy(out[0].extensionName,"XR_KHR_vulkan_enable2");return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* info,XrInstance* out) {
    check(info->enabledExtensionCount==1 && std::strcmp(info->enabledExtensionNames[0],"XR_KHR_vulkan_enable2")==0);
    ++creates;*out=reinterpret_cast<XrInstance>(uintptr_t(1));return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {check(instance!=XR_NULL_HANDLE);++destroys;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance,XrInstanceProperties* out) {std::strcpy(out->runtimeName,"test runtime");return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance,const XrSystemGetInfo* info,XrSystemId* out) {
    check(info->formFactor==XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY);if(fail_system) return XR_ERROR_FORM_FACTOR_UNAVAILABLE;*out=9;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance,XrSystemId,XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrViewConfigurationView* out) {
    check(type==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);*count=(capacity && change_eye_count)?1:eye_count;
    for(unsigned i=0;i<capacity;++i) out[i].recommendedImageRectWidth=1024;return XR_SUCCESS;
}
}
int main()try {
    using namespace starfox::vr;
    {
        OpenXrRuntime runtime;check(runtime.initialize());check(runtime.views().size()==2 && runtime.system()==9 && runtime.supports_vulkan());
        check(runtime.initialize() && creates==2 && destroys==1);
        fail_system=true;check(!runtime.initialize());check(creates==3 && destroys==3);
        check(runtime.instance()==XR_NULL_HANDLE && runtime.system()==XR_NULL_SYSTEM_ID && runtime.views().empty() && !runtime.supports_vulkan());
        fail_system=false;eye_count=1;check(!runtime.initialize() && creates==4 && destroys==4);
        eye_count=2;graphics=false;check(!runtime.initialize() && creates==4);graphics=true;
        AndroidXrContext context;check(!runtime.initialize(&context) && creates==4);
        check(runtime.initialize());
        change_eye_count=true;check(!runtime.initialize());
        check(runtime.instance()==XR_NULL_HANDLE && runtime.views().empty() && !runtime.supports_vulkan());
        change_eye_count=false;check(runtime.initialize());
    }
    check(creates==7 && destroys==7);std::cout<<"OpenXR runtime initialization, failure cleanup, reinitialization and destruction passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
