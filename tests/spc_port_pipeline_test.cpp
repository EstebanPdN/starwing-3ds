#include "SNES_SPC.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
struct Copy {
 std::vector<unsigned char> bytes;
 static void fields(unsigned char** io,void* data,std::size_t size) {
  auto& self=*reinterpret_cast<Copy*>(*io);auto* first=static_cast<unsigned char*>(data);
  self.bytes.insert(self.bytes.end(),first,first+size);
 }
};
auto state(SNES_SPC& spc) {Copy result;auto* io=reinterpret_cast<unsigned char*>(&result);spc.copy_state(&io,Copy::fields);return result.bytes;}
void require(bool b,const char* what){if(!b)throw std::runtime_error(what);}
int main()try {
 std::mt19937 random(0x3d506);unsigned cases{},accepted{},rejected{};unsigned long long events{};
 const std::vector<std::vector<unsigned char>> programs{
  {0xe8,0x42,0xc4,0x20,0xe4,0x20,0xc5,0xff,0xff,0xe5,0xff,0xff,
   0x2d,0xae,0x4d,0xce,0x6d,0xee,0x3f,0x80,0x02,0xc4,0xf5,
   0x8f,0x0c,0xf2,0x8f,0x7f,0xf3,0x8f,0x7d,0xf2,0xe4,0xf3},
  {0xe5,0x00,0xc0,0xc4,0xf5},
  {0xe8,0x12,0xc5,0x00,0xc0,0xc4,0xf5},
  {0x8f,0x7c,0xf2,0xe4,0xf3,0xc4,0xf5},
  {0x8f,0x08,0xf2,0xe4,0xf3,0xc4,0xf5},
  {0x8f,0x09,0xf2,0xe4,0xf3,0xc4,0xf5},
  {0x8f,0x6d,0xf2,0x8f,0x40,0xf3,0xe5,0x00,0x40,0xc4,0xf5},
  {0x8f,0x7d,0xf2,0x8f,0x0f,0xf3,0xe5,0x00,0xc8,0xc4,0xf5},
  {0x8f,0x6d,0xf2,0x8f,0x01,0xf3,0x2d,0xae,0xc4,0xf5},
  {0x8f,0x6d,0xf2,0x8f,0x02,0xf3,0xc4,0xf5},
  {0xcd,0x01,0xe8,0x51,0xd5,0xff,0xff,0xf5,0xff,0xff,0xc4,0xf5},
  {0x8f,0x80,0xf1,0x8f,0x00,0xf1,0xe5,0xff,0xff,0xc4,0xf5}
 };
 std::array<unsigned,12> rejected_by_program{};
 for(unsigned kind=0;kind<programs.size();++kind)for(unsigned trial=0;trial<32;++trial) {
  std::vector<unsigned char> snapshot(SNES_SPC::spc_file_size);SNES_SPC::init_header(snapshot.data());
  snapshot[0x25]=0;snapshot[0x26]=2;snapshot[0x27]=random();snapshot[0x28]=random();
  snapshot[0x29]=random();snapshot[0x2a]=0;snapshot[0x2b]=std::array<unsigned,4>{0,1,2,255}[trial%4];
  auto* ram=snapshot.data()+0x100;for(unsigned i=0;i<65536;++i)ram[i]=random();
  std::fill(ram+0xf0,ram+0x100,0);ram[0xf1]=0;ram[0xfa]=7;ram[0xfb]=11;ram[0xfc]=3;
  const unsigned char gate[]{0xe4,0xf4,0xf0,0xfc};std::copy(std::begin(gate),std::end(gate),ram+0x200);
  std::copy(programs[kind].begin(),programs[kind].end(),ram+0x204);
  const unsigned end=0x204+programs[kind].size();ram[end]=0x2f;ram[end+1]=static_cast<unsigned char>(0x204-(end+2));ram[0x280]=0x6f;
  snapshot[0x10100+0x6c]=0x20;snapshot[0x10100+0x6d]=0xc0;snapshot[0x10100+0x7d]=kind==8?0:1;
  auto ref=std::make_unique<SNES_SPC>(),candidate=std::make_unique<SNES_SPC>(),pred=std::make_unique<SNES_SPC>();
  require(!ref->init()&&!candidate->init(),"init");
  require(!ref->load_spc(snapshot.data(),snapshot.size())&&!candidate->load_spc(snapshot.data(),snapshot.size()),"load");
  candidate->enable_native_blocks(true);ref->enable_native_blocks(true);
  std::array<short,4096> a{},b{};ref->set_output(a.data(),a.size());candidate->set_output(b.data(),b.size());
  ref->end_frame(51200);candidate->end_frame(51200);
  for(unsigned slice=0;slice<16;++slice) {
   a.fill(0);b.fill(0);ref->set_output(a.data(),a.size());candidate->set_output(b.data(),b.size());
   const int clocks=slice==0?int(128+random()%256):int(1+random()%2048);
   ref->write_port(0,0,1);ref->write_port(clocks/2,2,trial);
   std::array<unsigned char,4> expected{},actual{};for(unsigned p=0;p<4;++p)expected[p]=ref->read_port(clocks,p);
   bool predicted=false;
   try {
    pred->prepare_port_pipeline(*candidate);pred->write_port(0,0,1);pred->write_port(clocks/2,2,trial);
    for(unsigned p=0;p<4;++p)actual[p]=pred->read_port(clocks,p);
    require(expected==actual,"predicted ports");events+=pred->pipeline_event_count();
    candidate->finish_port_pipeline(*pred);predicted=true;++accepted;
   }catch(const SNES_SPC::PortPipelineUnavailable&) {++rejected;++rejected_by_program[kind];}
   if(!predicted){candidate->write_port(0,0,1);candidate->write_port(clocks/2,2,trial);for(unsigned p=0;p<4;++p)actual[p]=candidate->read_port(clocks,p);}
   ref->end_frame(clocks);candidate->end_frame(clocks);
   if(expected!=actual||a!=b||ref->sample_count()!=candidate->sample_count()||state(*ref)!=state(*candidate)) {
    std::fprintf(stderr,"kind=%u trial=%u slice=%u predicted=%d\n",kind,trial,slice,predicted);throw std::runtime_error("PCM/state differs");
   }
   ++cases;
  }
 }
 for(unsigned kind=1;kind<=9;++kind)require(rejected_by_program[kind]>0,"hazard guard not exercised");
 require(accepted>500&&rejected>500,"insufficient branch coverage");
 std::printf("PASS cases=%u accepted=%u rejected=%u replayed_events=%llu; PCM/state/ports, echo RAM/code/stack, ESA/EDL, ENDX/ENVX/OUTX, stack wrap, ROM and address wrap\n",cases,accepted,rejected,events);
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
