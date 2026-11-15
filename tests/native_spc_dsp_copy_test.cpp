#include <SNES_SPC.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
struct Copy {
    std::vector<std::uint8_t> bytes;
    static void fields(unsigned char** io,void* data,std::size_t size) {
        auto& self=*reinterpret_cast<Copy*>(*io);
        auto* first=static_cast<std::uint8_t*>(data);
        self.bytes.insert(self.bytes.end(),first,first+size);
    }
};
auto state(SNES_SPC& spc) {
    Copy result;auto* io=reinterpret_cast<unsigned char*>(&result);
    spc.copy_state(&io,Copy::fields);return result.bytes;
}
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
}
int main()try {
 std::mt19937 random(0x3d5a461);unsigned cases{};unsigned long long native{};
 for(unsigned fixture=0;fixture<64;++fixture) {
  std::vector<std::uint8_t> snapshot(SNES_SPC::spc_file_size);SNES_SPC::init_header(snapshot.data());
  snapshot[0x25]=0x61;snapshot[0x26]=4;snapshot[0x27]=random();snapshot[0x28]=random();
  snapshot[0x29]=std::array<std::uint8_t,4>{0,1,10,255}[fixture%4];snapshot[0x2a]=(fixture&4)?0x20:0;snapshot[0x2b]=0xff;
  auto* ram=snapshot.data()+0x100;
  for(unsigned i=0;i<65536;++i)ram[i]=random();
  std::memset(ram+0xf0,0,16);
  ram[0x453]=0x2f;ram[0x454]=12;
  constexpr std::array<std::uint8_t,18> code{0xf6,0x9b,0x0e,0xc5,0xf2,0x00,0xf6,0xa5,0x0e,0x5d,0xe6,0xc5,0xf3,0x00,0xfe,0xe2,0x2f,0xee};
  std::copy(code.begin(),code.end(),ram+0x461);
  if(fixture>=16&&fixture<32)ram[0x461]=0xf5;
  if(fixture>=32&&fixture<48)ram[0x465]=0xf4;
  // Read timer and DSP ports via (X) as well as plain direct-page RAM.
  for(unsigned i=0;i<256;++i)ram[0xea5+i]=std::uint8_t(0xf0+i%16);
  snapshot[0x10100+0x6c]=fixture>=48?0:0x20;
  snapshot[0x10100+0x6d]=4;snapshot[0x10100+0x7d]=1;
  SNES_SPC reference,candidate;
  require(!reference.init()&&!candidate.init(),"SPC init");
  require(!reference.load_spc(snapshot.data(),snapshot.size())&&!candidate.load_spc(snapshot.data(),snapshot.size()),"SPC load");
  candidate.enable_native_blocks(true);
  for(unsigned slice=0;slice<128;++slice) {
   std::array<short,1024> a{},b{};reference.set_output(a.data(),a.size());candidate.set_output(b.data(),b.size());
   const int clocks=slice<64?int(slice+1):int(1+random()%512);
   reference.end_frame(clocks);candidate.end_frame(clocks);
   if(a!=b||reference.sample_count()!=candidate.sample_count()||state(reference)!=state(candidate)) {
    std::cerr<<"fixture="<<fixture<<" slice="<<slice<<" clocks="<<clocks<<'\n';throw std::runtime_error("DSP-copy native state or PCM mismatch");
   }
   ++cases;
  }
  if(fixture>=16&&fixture<48)require(candidate.native_block_count()==0,"Mutated code did not fall back");
  native+=candidate.native_block_count();
 }
#ifdef NDEBUG
 require(native>100,"DSP-copy block was not exercised");
#endif
 std::cout<<"PASS "<<cases<<" exact PCM/core comparisons, "<<native<<" native DSP-copy blocks; deadlines, widths, ports, timers, echo RAM and code mutation\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
