#include "starfox/audio/brr_decode.hpp"
#include <array>
#include <cstdio>
#include <random>
#include <stdexcept>
void reference(unsigned nybbles,unsigned header,int* pos) {
 for(unsigned n=0;n<4;++n,nybbles<<=4) {
  int s=static_cast<int16_t>(nybbles)>>12;
  const unsigned shift=header>>4;
  s=(s*(1<<shift))>>1;
  if(shift>=13)s=s<0?-2048:0;
  int filter=header&12,p1=pos[n+11],p2=pos[n+10]>>1;
  if(filter>=8) {
   s+=p1;s-=p2;
   if(filter==8){s+=p2>>4;s+=(p1*-3)>>6;}
   else{s+=(p1*-13)>>7;s+=(p2*3)>>4;}
  }else if(filter){s+=p1>>1;s+=(-p1)>>5;}
  s=static_cast<int16_t>(starfox::audio::saturate16(s)*2);
  pos[n]=pos[n+12]=s;
 }
}
int main()try {
 std::mt19937 random(0x3d5b776);unsigned cases=0;
 for(unsigned header=0;header<256;++header)for(unsigned trial=0;trial<8192;++trial) {
  std::array<int,24>a,b;for(auto& v:a)v=static_cast<int16_t>(random());b=a;
  unsigned data=random()&65535;
  if(trial<16) {a[10]=b[10]=trial&1?-32768:32767;a[11]=b[11]=trial&2?-32768:32767;data=0x1111U*(trial&15U);}
  reference(data,header,a.data());starfox::audio::decode_brr_four(data,header,b.data());
  if(a!=b)throw std::runtime_error("BRR output differs");++cases;
 }
 std::printf("PASS BRR groups=%u samples=%u all headers, predictors and saturation corners\n",cases,cases*4U);
 return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}
