#pragma once
#include <exception>

namespace starfox::platform_3ds {
// Let independent simulation work overlap the outstanding SPC packet. The
// first APU output read still sees exactly the same published port values as
// an eager wait. Bank-upload writes fence too; ordinary queued driver writes
// can proceed without waiting for the output latch.
template<class Map,class Audio>
class DeferredApuSync {
public:
    DeferredApuSync(Map& map,Audio& audio):map_(map),audio_(audio) {
        if(audio_.pending())map_.set_apu_access_fence(&access,this);
    }
    ~DeferredApuSync(){map_.set_apu_access_fence(nullptr,nullptr);}
    DeferredApuSync(const DeferredApuSync&)=delete;
    DeferredApuSync& operator=(const DeferredApuSync&)=delete;
    void check_failure() const {if(failure_)std::rethrow_exception(failure_);}
private:
    static void access(void* context) noexcept {
        auto& self=*static_cast<DeferredApuSync*>(context);
        try {
            if(self.audio_.pending()) {
                self.map_.set_apu_output_ports(self.audio_.wait_for_output_ports());
            }
        } catch(...) {
            // Map's read API is noexcept. Deliver the audio failure to the
            // frontend immediately after its bounded simulation tick instead.
            self.failure_=std::current_exception();
        }
    }
    Map& map_;
    Audio& audio_;
    std::exception_ptr failure_;
};
}
