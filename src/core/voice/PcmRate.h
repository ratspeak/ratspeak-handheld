#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace handheld::voice {
// Board PCM is 16 kHz; native Codec2 uses 8 kHz. No allocation or clock state
// is shared with notification audio. Reset on every direction/capture change.
class PcmRate {
public:
    void reset() {std::memset(_history,0,sizeof _history);_position=0;_last=0;}
    void down(const int16_t* input,int16_t* output,size_t pairs) {
        // 31-tap Hamming low-pass, 3.44 kHz cutoff, normalized Q15.
        static constexpr int16_t taps[]={55,4,-92,-70,165,261,-183,-629,-27,1147,728,-1701,-2503,2129,10064,14072,10064,2129,-2503,-1701,728,1147,-27,-629,-183,261,165,-70,-92,4,55};
        for(size_t n=0;n<pairs*2;++n) {
            _history[_position]=input[n];
            if(n&1) {
                int64_t sum=0;for(unsigned k=0;k<31;++k) sum+=int64_t(taps[k])*_history[(_position+31-k)%31];
                sum/=32768;output[n/2]=int16_t(sum>32767?32767:sum< -32768?-32768:sum);
            }
            _position=(_position+1)%31;
        }
    }
    void up(const int16_t* input,int16_t* output,size_t samples,uint8_t volume) {
        if(volume>100) volume=100;
        for(size_t n=0;n<samples;++n) {
            const int16_t value=int32_t(input[n])*volume/100;
            output[2*n]=(int32_t(_last)+value)/2;output[2*n+1]=value;_last=value;
        }
    }
private:
    int16_t _history[31]{},_last=0;
    uint8_t _position=0;
};
}
