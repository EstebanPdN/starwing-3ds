// Included inside SNES_SPC. Compute its CPU timeline while recording ordered
// DSP calls and RAM stores. The echo/register guards make this independent of
// DSP-generated values; any uncertain dependency discards the speculative
// work before publication. Replay changes the original instance exactly once.
public:
    struct PortPipelineUnavailable { unsigned reason,address; };
    void prepare_port_pipeline(const SNES_SPC& source) {
        m=source.m;dsp=source.dsp;
        pipeline_events_.clear();
        if(pipeline_events_.capacity()<8192)pipeline_events_.reserve(8192);
        native_block_count_=0;
        native_blocks_=source.native_blocks_;pipeline_enabled_=true;
        for(unsigned i=0;i<256;++i){pipeline_echo_pages_[i]=0;pipeline_echo_starts_[i]=0;}
        unsigned current,next,pointer;
        dsp.pipeline_echo_guard(current,next,pipeline_echo_length_,pointer);
        pipeline_echo_starts_[current]=pipeline_echo_starts_[next]=1;
        pipeline_mark_echo();
        pipeline_echo_pages_[pointer>>8U]=1;
        pipeline_echo_pages_[((pointer+3U)&65535U)>>8U]=1;
        // All stack accesses stay in this page, including wrapping RET/POP.
        pipeline_touch(0x100U);
    }
    void pipeline_touch(unsigned address) const {
        if(pipeline_enabled_&&pipeline_echo_pages_[(address&65535U)>>8U])
            throw PortPipelineUnavailable{1,address&65535U};
    }
    template<bool Predict> unsigned pipeline_read_code(const uint8_t* address) const {
        pipeline_touch_cpu<Predict>(unsigned(address-m.ram.ram));return *address;
    }
    template<bool Predict> unsigned pipeline_read_code_word(const uint8_t* address) const {
        pipeline_touch_cpu<Predict>(unsigned(address-m.ram.ram));
        pipeline_touch_cpu<Predict>(unsigned(address-m.ram.ram)+1U);
        return unsigned(address[0])|(unsigned(address[1])<<8U);
    }
    template<bool Predict> void pipeline_touch_cpu(unsigned address) const {
        if constexpr(Predict)if(pipeline_echo_pages_[(address&65535U)>>8U])
            throw PortPipelineUnavailable{1,address&65535U};
    }
    template<bool Predict> int pipeline_cpu_read(int address,rel_time_t time) {
        pipeline_touch_cpu<Predict>(unsigned(address));return cpu_read(address,time);
    }
    template<bool Predict> void pipeline_cpu_write(int data,int address,rel_time_t time) {
        pipeline_touch_cpu<Predict>(unsigned(address));cpu_write(data,address,time);
    }
    void pipeline_touch_word(unsigned address) const {
        pipeline_touch(address);pipeline_touch(address+1U);
    }
    void pipeline_dsp_read(unsigned reg) const {
        if(pipeline_enabled_&&((reg&15U)==8U||(reg&15U)==9U||reg==0x7cU))
            throw PortPipelineUnavailable{2,reg};
    }
    void pipeline_dsp_write(unsigned reg,unsigned value) {
        if(!pipeline_enabled_)return;
        if(reg==0x6dU)pipeline_echo_starts_[value&255U]=1;
        else if(reg==0x7dU) {
            const unsigned length=(value&15U)*2048U;
            if(length>pipeline_echo_length_)pipeline_echo_length_=length;
        }else return;
        pipeline_mark_echo();
        pipeline_touch(0x100U);
    }
private:
    bool pipeline_enabled_=false;
    unsigned pipeline_echo_length_{};
    unsigned char pipeline_echo_pages_[256]{};
    unsigned char pipeline_echo_starts_[256]{};
    void pipeline_mark_echo() {
        if(pipeline_echo_length_>=65536U) {
            for(auto& page:pipeline_echo_pages_)page=1;
            return;
        }
        for(unsigned start=0;start<256;++start)if(pipeline_echo_starts_[start])
            for(unsigned offset=0;offset<pipeline_echo_length_;offset+=256U)
                pipeline_echo_pages_[(start+(offset>>8U))&255U]=1;
    }
    std::vector<unsigned> pipeline_events_;
    void pipeline_event(unsigned value) {
        if(pipeline_events_.size()==65536U)throw PortPipelineUnavailable{3,0};
        pipeline_events_.push_back(value);
    }
    void pipeline_run_dsp(int count) {
        if(pipeline_enabled_)pipeline_event(0x40000000U|unsigned(count));
        else dsp.run(count);
    }
    void pipeline_write_dsp(unsigned reg,unsigned value) {
        if(pipeline_enabled_)pipeline_event(0x80000000U|(reg<<8U)|(value&255U));
        dsp.write(reg,value);
    }
    void pipeline_store(unsigned address,unsigned value) {
        m.ram.ram[address]=uint8_t(value);
        if(pipeline_enabled_&&address<65536U)pipeline_event((address<<8U)|(value&255U));
    }
    template<bool Predict> void pipeline_store_cpu(unsigned address,unsigned value) {
        m.ram.ram[address]=uint8_t(value);
        if constexpr(Predict)if(address<65536U)pipeline_event((address<<8U)|(value&255U));
    }
    template<bool Predict> void pipeline_store_word_cpu(uint8_t* address,unsigned value) {
        pipeline_store_cpu<Predict>(unsigned(address-m.ram.ram),value);
        pipeline_store_cpu<Predict>(unsigned(address-m.ram.ram)+1U,value>>8U);
    }
public:
    unsigned pipeline_event_count() const {return unsigned(pipeline_events_.size());}
    void finish_port_pipeline(const SNES_SPC& predicted) {
        for(const unsigned event:predicted.pipeline_events_) {
            switch(event>>30U) {
            case 0:m.ram.ram[(event>>8U)&65535U]=uint8_t(event);break;
            case 1:dsp.run(int(event&0x3fffffffU));break;
            case 2:dsp.write((event>>8U)&127U,event&255U);break;
            }
        }
        // The output buffers/carry and DSP belong to this instance. Copy
        // only CPU/timer and ROM bookkeeping; replay already applied RAM
        // stores in order around DSP calls, including the DSP's echo stores.
        std::memcpy(&m,&predicted.m,offsetof(state_t,extra_clocks));
        std::memcpy(&m.rom_enabled,&predicted.m.rom_enabled,
            offsetof(state_t,ram)-offsetof(state_t,rom_enabled));
        native_block_count_+=predicted.native_block_count_;
    }
private:
