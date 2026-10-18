#include "audio_3ds.hpp"
#include "hardware_profile.hpp"
#include <stdexcept>
#include <iostream>
using namespace starfox::platform_3ds;
void require(bool ok,const char* m){if(!ok)throw std::runtime_error(m);}
int main(){
    for(unsigned model=0;model<3;++model){
        audio_test::new_3ds=model==1;audio_test::model_result=model==2?-1:0;
        audio_test::quota_calls=0;audio_test::requested_cores.clear();
        Audio3ds audio;require(audio.open(),"Backend did not open");
        const auto route=audio.spc().routing();
        if(model==1){
            require(route==starfox::audio::Spc700Audio::Routing::split_stems,"New routing changed");
            require(!audio_test::requested_cores.empty()&&audio_test::requested_cores[0]==2,"New did not try core 2");
        }else{
            require(route==starfox::audio::Spc700Audio::Routing::cartridge,"Old/unknown routing is not single SPC");
            require(audio.worker_core()==0&&audio_test::quota_calls==0&&audio_test::requested_cores.empty(),"Old requested a constrained worker");
        }
    }
    std::cout<<"PASS: actual Audio3ds selects single SPC without workers on Old/unknown; New retains split/core-2 attempt\n";
}
