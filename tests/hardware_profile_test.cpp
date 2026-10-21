#include "audio_3ds.hpp"
#include "hardware_profile.hpp"
#include <stdexcept>
#include <iostream>
using namespace starfox::platform_3ds;
void require(bool ok,const char* m){if(!ok)throw std::runtime_error(m);}
int main(){
    require(hardware_profile(false).pica_world && hardware_profile(true).pica_world,"World renderer policy changed");
    // model 0 = Old, 1 = New, 2 = unknown (failed query). Old/unknown runs one
    // cartridge SPC on core 1 when APT grants a slice and the thread starts;
    // otherwise it computes on the application core with the quota restored.
    for(unsigned model=0;model<3;++model){
        for(unsigned outcome=0;outcome<3;++outcome){
            audio_test::new_3ds=model==1;audio_test::model_result=model==2?-1:0;
            audio_test::quota_calls=0;audio_test::requested_cores.clear();
            audio_test::quota_requests.clear();audio_test::cpu_quota=30U;
            audio_test::honour_quota=outcome!=0;audio_test::thread_succeeds=outcome==2;
            {
            Audio3ds audio;require(audio.open(),"Backend did not open");
            const auto route=audio.spc().routing();
            if(model==1){
                require(route==starfox::audio::Spc700Audio::Routing::split_stems,"New routing changed");
                require(!audio_test::requested_cores.empty()&&audio_test::requested_cores[0]==2,"New did not try core 2");
                if(outcome==2) require(audio.worker_core()==2&&audio_test::quota_calls==0,"New core-2 worker took a core-1 slice");
                continue;
            }
            require(route==starfox::audio::Spc700Audio::Routing::cartridge,"Old/unknown routing is not single SPC");
            for(const auto core:audio_test::requested_cores) require(core==1,"Old requested a core other than 1");
            require(!audio_test::quota_requests.empty()&&audio_test::quota_requests[0]==80U,"Old did not request an 80% slice first");
            if(outcome==2){
                require(audio.worker_core()==1&&audio.cpu_quota_percent()==80U,"Old did not start its core-1 worker");
            }else{
                require(audio.worker_core()==0,"Old without a worker is not synchronous");
                require(audio_test::requested_cores.size()==(outcome==1?3U:0U),"Old core-1 attempts changed");
                if(audio_test::honour_quota) require(audio_test::cpu_quota==30U,"Old did not restore the prior slice");
            }
            }
            if(model!=1&&outcome==2) require(audio_test::cpu_quota==30U,"Closing did not restore the prior slice");
        }
    }
    std::cout<<"PASS: Old/unknown runs one cartridge SPC on a granted core-1 slice or synchronously, restoring APT's slice; New retains split/core-2\n";
}
