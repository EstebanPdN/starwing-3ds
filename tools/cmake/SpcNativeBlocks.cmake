# Lift one RAM-only SPC instruction pattern; retain the pinned interpreter.
set(native_header_anchor "    int sample_count() const;")
string(FIND "${core_header}" "${native_header_anchor}" native_header_position)
if(native_header_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC native block interface needs review")
endif()
string(REPLACE "${native_header_anchor}" [=[
    int sample_count() const;
    void enable_native_blocks(bool enabled) { native_blocks_ = enabled; }
    unsigned long long native_block_count() const { return native_block_count_; }
private:
    bool native_blocks_ = false;
    unsigned long long native_block_count_ = 0;
public:
]=] core_header "${core_header}")
starfox_write_spc_file("SNES_SPC.h" "${core_header}")

file(READ "${snes_spc_SOURCE_DIR}/snes_spc/SPC_CPU.h" cpu_source)
string(REPLACE "uint8_t* const ram = RAM;" "uint8_t* const ram = RAM;\n    uint8_t const* const native_entry = native_blocks_ ? ram+0x624 : 0;" cpu_source "${cpu_source}")
set(native_anchor "\topcode = *pc;")
string(FIND "${cpu_source}" "${native_anchor}" native_position)
if(native_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC native block entry needs review")
endif()
string(REPLACE "${native_anchor}" [=[
    #if !SPC_MORE_ACCURACY && defined(NDEBUG)
    // No I/O, ROM writes, self-modification or event boundary may enter here.
    if (pc == native_entry && rel_time <= -53
        && pc[0]==0xaa && pc[3]==0x8a && pc[6]==0x2b && pc[8]==0x2b
        && pc[10]==0xe4 && pc[12]==0x25 && pc[15]==0x08 && pc[17]==0xd6
        && pc[20]==0xfc && pc[21]==0xe4 && pc[23]==0x08 && pc[25]==0xd6
        && pc[28]==0xfc && pc[29]==0x1d && pc[30]==0xd0 && pc[31]==0xe0)
    {
        unsigned bit0=GET_LE16(pc+1), bit1=GET_LE16(pc+4);
        unsigned and_addr=GET_LE16(pc+13);
        unsigned base0=GET_LE16(pc+18), base1=GET_LE16(pc+26);
        unsigned write0=base0+y;
        unsigned write1=base1+uint8_t(y+1);
        unsigned code_addr=GET_PC();
        if (unsigned((bit0&0x1fff)-0xf0)>=16 && unsigned((bit1&0x1fff)-0xf0)>=16
            && unsigned(and_addr-0xf0)>=16
            && (dp || (pc[7]<0xf0 && pc[9]<0xf0 && pc[11]<0xf0 && pc[22]<0xf0))
            && unsigned(write0-0x200)<0xfdc0 && unsigned(write1-0x200)<0xfdc0
            && unsigned(write0-code_addr)>=32 && unsigned(write1-code_addr)>=32)
        {
            // If every possible Y-indexed destination is ordinary RAM and
            // excludes this code, all loop operands remain stable until X
            // reaches zero. Execute the whole clock-bounded native loop
            // after one pattern check, not another dispatch/check per pair
            // of generated samples. Other layouts keep the single-iteration
            // guard, including aliases that can mutate later code operands.
            unsigned batch = unsigned(base0-0x200)<0xfdc0-255
                && unsigned(base1-0x200)<0xfdc0-255
                && (base0+255<code_addr || base0>=code_addr+32)
                && (base1+255<code_addr || base1>=code_addr+32) ? 256 : 1;
            unsigned iterations=0;
            do {
            c=(((ram[bit0&0x1fff]>>(bit0>>13))
                ^ (ram[bit1&0x1fff]>>(bit1>>13)))<<8)&0x100;
            unsigned rotate0=ram[dp+pc[7]]<<1;
            ram[dp+pc[7]]=uint8_t(rotate0|(c>>8&1));c=rotate0;
            unsigned rotate1=ram[dp+pc[9]]<<1;
            ram[dp+pc[9]]=uint8_t(rotate1|(c>>8&1));c=rotate1;
            a=(ram[dp+pc[11]]&ram[and_addr])|pc[16];
            ram[base0+y]=uint8_t(a);y=uint8_t(y+1);
            a=ram[dp+pc[22]]|pc[24];
            ram[base1+y]=uint8_t(a);y=uint8_t(y+1);
            nz=x-1;x=uint8_t(nz);
            rel_time+=53;
            ++iterations;
            if (!x) { rel_time-=2;pc+=32;break; }
            } while(iterations<batch && rel_time<=-53);
            native_block_count_+=iterations;
            goto loop;
        }
    }

    #endif
    opcode = *pc;
]=] cpu_source "${cpu_source}")
# Lift the driver's DSP-register copy without moving any register timestamp.
set(dsp_copy_anchor "    opcode = *pc;")
string(FIND "${cpu_source}" "${dsp_copy_anchor}" dsp_copy_position)
if(dsp_copy_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC DSP-copy block entry needs review")
endif()
string(REPLACE "${dsp_copy_anchor}" [=[
    #if !SPC_MORE_ACCURACY && defined(NDEBUG)
    // Driver DSP-register copy: preserve the timestamp of every port access.
    // A DSP catch-up may write echo RAM, so recheck the final branch afterwards.
    static uint8_t const dsp_copy_code[16]={0xf6,0x9b,0x0e,0xc5,0xf2,0x00,
        0xf6,0xa5,0x0e,0x5d,0xe6,0xc5,0xf3,0x00,0xfe,0xe2};
    if(native_blocks_ && pc==ram+0x461 && rel_time<=-31
        && !memcmp(pc,dsp_copy_code,16)) {
        rel_time+=5; a=nz=READ(0,0x0e9b+y);
        rel_time+=5; WRITE(0,0xf2,a);
        rel_time+=5; a=nz=READ(0,0x0ea5+y);
        rel_time+=2; x=nz=a;
        rel_time+=3; a=nz=READ(0,dp+x);
        if(pc[11]!=0xc5||pc[12]!=0xf3||pc[13]!=0) {pc+=11;goto loop;}
        rel_time+=5; WRITE(0,0xf3,a);
        pc+=14;
        ++native_block_count_;
        if(pc[0]!=0xfe||pc[1]!=0xe2)goto loop;
        rel_time+=6;y=uint8_t(y-1);
        if(y)pc-=28;
        else {pc+=2;rel_time-=2;}
        goto loop;
    }
    #endif
    opcode = *pc;
]=] cpu_source "${cpu_source}")
starfox_write_spc_file("SPC_CPU.h" "${cpu_source}")
