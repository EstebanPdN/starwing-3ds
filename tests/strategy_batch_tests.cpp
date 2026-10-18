#include "starfox/assets/rom.hpp"
#include "starfox/simulation/map_vm.hpp"
#include "starfox/simulation/object_pool.hpp"
#include "starfox/simulation/strategy_scheduler.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace starfox;
using namespace starfox::simulation;
namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
struct Code {
    std::vector<std::uint8_t> bytes;
    void emit(std::initializer_list<std::uint8_t> values) { bytes.insert(bytes.end(),values); }
    void address(std::uint32_t a) { emit({std::uint8_t(a),std::uint8_t(a>>8),std::uint8_t(a>>16)}); }
    void byte(std::uint32_t a,std::uint8_t v) { emit({0xa9,v,0x8f});address(a); }
    void word(std::uint32_t a,std::uint16_t v) {
        emit({0xc2,0x20,0xa9,std::uint8_t(v),std::uint8_t(v>>8),0x8f});address(a);emit({0xe2,0x20});
    }
    void pointer(std::uint32_t a,std::uint32_t v) { word(a,std::uint16_t(v));byte(a+2,std::uint8_t(v>>16)); }
    void call(std::uint32_t a) { emit({0x22});address(a); }
    void x(std::uint16_t x) { emit({0xa2,std::uint8_t(x),std::uint8_t(x>>8)}); }
};
struct Machine {
    ObjectPool objects;
    MapVm map;
    NativeStrategyScheduler scheduler;
    Machine(const assets::RomImage& rom,const assets::SymbolMap& symbols)
        :objects(symbols.find("NUMBER_AL").at(0),symbols.find("AL_SIZE").at(0)==57
            ?ObjectMemoryLayout::starfox_ex:ObjectMemoryLayout::original),
         map(rom,MapDatabase(rom,symbols),objects,&symbols),scheduler(symbols,objects,map,2000) {}
};
void compare(const Machine& before,const Machine& after,const std::string& label) {
    require(before.objects.save_state()==after.objects.save_state(),label+" object pool/generations");
    require(before.map.save_state()==after.map.save_state(),label+" full CPU/VM state");
}
void run_case(const assets::RomImage& base_rom,const assets::SymbolMap& symbols,unsigned test) {
    const auto sym=[&](const char* n){return symbols.find(n).at(0);};
    const auto base=sym("ALBLKS"),stride=sym("AL_SIZE");
    const auto ptr=[&](unsigned h){return std::uint16_t(base+(h-1)*stride);};
    constexpr std::uint32_t count_code=0x7e7000,action_code=0x7e7100,counter=0x7e7800;
    Code action;
    unsigned actor=1;
    if(test==1) { // Earlier/later payloads, flags, parent-like and collision references.
        actor=4;
        action.word(0x7e0000+ptr(2)+12,0x8f21);action.word(0x7e0000+ptr(7)+16,0x45ab);
        action.word(0x7e0000+ptr(2)+6,ptr(8));action.word(0x7e0000+ptr(7)+25,ptr(3));
        action.word(0x7e0000+ptr(2)+27,ptr(9));
        action.word(0x7e2000+(7-1)*stride+19,ptr(2));
        action.byte(0x7e0000+ptr(2)+8,0x51);
    } else if(test==2) { // A future object's strategy changes before it is visited.
        action.pointer(0x7e0000+ptr(7)+22,action_code+0x200);
    } else if(test==3) { actor=5; action.call(sym("REMOVEDEADAL_L"));
    } else if(test==4) { // Remove the next object and then self.
        actor=4;action.emit({0xda});action.x(ptr(5));action.call(sym("REMOVEDEADAL_L"));
        action.emit({0xfa});action.call(sym("REMOVEDEADAL_L"));
    } else if(test==5) { actor=5;action.byte(sym("ALDEAD"),1);
    } else if(test==6 || test==7) {
        // Insert either the first unused slot or a slot removed earlier this tick.
        actor=3;const unsigned born=test==6?11:1;
        action.word(0x7e0000+ptr(born),ptr(4));action.word(0x7e0000+ptr(born)+2,ptr(3));
        action.word(0x7e0000+ptr(3),ptr(born));action.word(0x7e0000+ptr(4)+2,ptr(born));
        action.word(sym("ALFREELST"),ptr(test==6?12:11));
        action.pointer(0x7e0000+ptr(born)+22,count_code);
        action.byte(0x7e0000+ptr(born)+sym("AL_HP"),1);
        action.word(0x7e0000+ptr(born)+6,ptr(8));
    } else if(test==8 || test==9) {
        // Commit other-object writes, then escape into unmapped ROM. Test 9
        // puts this in the path-controlled range to exercise recovery/removal.
        actor=5;action.word(0x7e0000+ptr(2)+12,0x1234);
        action.word(0x7e0000+ptr(7)+6,ptr(2));action.emit({0x5c,0x00,0x80,0x6f});
    } else if(test==10) {
        // The escape-camera special case must read a just-changed strategy.
        action.pointer(0x7e0000+ptr(7)+22,sym("VIEWOUTOFLB1_STRAT"));
        action.byte(sym("GAMEFLAGS2"),1);action.word(sym("MAPVAR1"),0);
    }
    action.emit({0x6b});
    auto rom_bytes=base_rom.bytes();
    const auto action_entry=test==9?sym("PATHDHA_ISTRAT"):action_code;
    if(test==9) std::copy(action.bytes.begin(),action.bytes.end(),
        rom_bytes.begin()+base_rom.lorom_offset(action_entry));
    const assets::RomImage rom(std::move(rom_bytes));
    auto fixture=std::make_unique<Machine>(rom,symbols);
    ObjectHandle tail=0;
    for(unsigned i=0;i<(test==11?5:10);++i) {
        tail=fixture->objects.allocate_after(tail);
        fixture->objects.at(tail).strategy_address=count_code;fixture->objects.at(tail).health=1;
    }
    const std::array<std::uint8_t,4> counter_bytes{0xee,0x00,0x78,0x6b};
    for(unsigned i=0;i<counter_bytes.size();++i)fixture->map.write_native_byte(count_code+i,counter_bytes[i]);
    if(test!=9)for(unsigned i=0;i<action.bytes.size();++i)fixture->map.write_native_byte(action_entry+i,action.bytes[i]);
    for(unsigned i=0;i<counter_bytes.size();++i)fixture->map.write_native_byte(action_code+0x200+i,counter_bytes[i]);
    if(test!=0 && test!=11)fixture->objects.at(actor).strategy_address=action_entry;
    if(test==12) {
        // The first guard precedes the initial host-to-WRAM push. It must
        // observe the host strategy even when WRAM still holds another one.
        fixture->objects.at(1).strategy_address=sym("VIEWOUTOFLB1_STRAT");
        fixture->map.write_native_byte(sym("GAMEFLAGS2"),1);
        fixture->map.write_native_word(sym("MAPVAR1"),0);
        fixture->map.mark_object_pool_host_modified();
    }
    if(test==7) {
        Code remove;remove.emit({0xda});remove.x(ptr(1));remove.call(sym("REMOVEDEADAL_L"));remove.emit({0xfa,0x6b});
        for(unsigned i=0;i<remove.bytes.size();++i)fixture->map.write_native_byte(action_code+0x300+i,remove.bytes[i]);
        fixture->objects.at(2).strategy_address=action_code+0x300;
    }
    auto reference=std::make_unique<Machine>(rom,symbols);
    reference->objects.load_state(fixture->objects.save_state());reference->map.load_state(fixture->map.save_state());
    StrategyTickStats a{},b{};std::string ea,eb;
    try {a=reference->scheduler.tick_all(false,false);}catch(const std::exception& e){ea=e.what();}
    try {b=fixture->scheduler.tick_all(false,true);}catch(const std::exception& e){eb=e.what();}
    const auto label="case "+std::to_string(test);
    require(ea==eb,label+" error/recovery context");
    if(!ea.empty())std::cerr<<label<<" caught: "<<ea<<'\n';
    if(test!=8 && test!=10 && test!=12)require(ea.empty(),label+" unexpected exception");
    require(a.instructions==b.instructions && a.objects_run==b.objects_run
        && a.objects_removed==b.objects_removed,label+" source counts");
    compare(*reference,*fixture,label);
    if(test==0) {
        const auto& full=reference->map.strategy_batch_counters();
        const auto& batch=fixture->map.strategy_batch_counters();
        require(full.payload_passes==10 && batch.payload_passes==1
            && batch.batched_calls==10 && batch.payload_slots==10,
            "batch counters did not record avoided full payload traversals");
        require(batch.checkpoint_bytes==10U*10U*stride*2U,
            "checkpoint counter did not match bounded active prefix copies");
    }
    if(test==11)require(fixture->map.strategy_batch_counters().batches==0,
        "small-list fallback unexpectedly enabled a payload batch");
    if(test==6)require(fixture->objects.is_active(11),"spawn fixture did not allocate slot");
    if(test==7)require(fixture->objects.generation(1)>10,"reuse fixture did not advance generation");
    if(test==8)require(!ea.empty(),"exception fixture did not throw");
    if(test==9)require(!fixture->objects.is_active(5),"path failure fixture did not recover");
    std::cout<<label<<" objects="<<b.objects_run<<" instructions="<<b.instructions<<" exact\n";
}
}
int main(int argc,char** argv) try {
    require(argc==3,"usage: strategy_batch_tests ROM SYMBOLS");
    const auto rom=assets::RomImage::load(argv[1]);const auto symbols=assets::SymbolMap::load(argv[2]);
    for(unsigned test=0;test<13;++test)run_case(rom,symbols,test);
    std::cout<<"strategy batch: 13 exact mutation/recovery fixtures passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
