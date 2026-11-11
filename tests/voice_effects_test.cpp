#include "voice_effects.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

static float rms(const std::vector<float>& x, size_t start=0) {
    double sum=0; for(size_t i=start;i<x.size();++i) sum+=x[i]*x[i];
    return static_cast<float>(std::sqrt(sum/(x.size()-start)));
}
static std::vector<float> tone(float hz, size_t count=48000) {
    std::vector<float> x(count);
    for(size_t i=0;i<count;++i) x[i]=.2f*std::sin(6.283185307179586*hz*i/48000);
    return x;
}
template<class F> void waitFor(F f) {
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while (!f()) { assert(std::chrono::steady_clock::now()<until); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
}
int main(int argc,char** argv) {
    assert(argc==2);
    auto dir=std::filesystem::path(argv[1]).wstring();
    VoiceDsp dsp(dir);
    VoiceSettings bypass; bypass.noise=false; bypass.gate=false;
    auto x=tone(180,12345);
    auto y=dsp.render(x,bypass);
    assert(x==y);
    assert(dsp.render({},bypass).empty());
    // Arbitrary phone packet sizes must preserve every sample across 512-hop boundaries.
    dsp.reset(); std::vector<float> streamed;
    for(size_t i=0;i<x.size();) {
        auto n=std::min<size_t>((i%3+1)*120,x.size()-i);
        auto out=dsp.process(x.data()+i,n,bypass); streamed.insert(streamed.end(),out.begin(),out.end()); i+=n;
    }
    assert(streamed.size()==x.size()/512*512);
    assert(std::equal(streamed.begin(),streamed.end(),x.begin()));
    VoiceSettings low=bypass; low.lowCut=true;
    auto rumble=tone(25); auto filtered=dsp.render(rumble,low);
    assert(rms(filtered,24000)<.12f*rms(rumble,24000));
    auto voice=tone(300); filtered=dsp.render(voice,low);
    assert(std::abs(rms(filtered,24000)/rms(voice,24000)-1)<.03);
    // The dry/wet blend must line up after compensating the 32 ms model delay.
    VoiceSettings full=bypass; full.noise=true;
    auto wet=dsp.render(x,full);
    VoiceSettings blend=full; blend.strength=20;
    auto mixed=dsp.render(x,blend);
    for(size_t i=0;i<x.size();++i) {
        assert(std::isfinite(wet[i]));
        assert(std::abs(mixed[i]-(.9f*wet[i]+.1f*x[i]))<1e-5);
    }
    // Real VAD must eventually close on sustained silence and reset reproducibly.
    auto silence=std::vector<float>(48000,0.f);
    VoiceSettings gated; gated.noise=false;
    assert(rms(dsp.render(silence,gated))==0);
    std::mt19937 rng(42); std::normal_distribution<float> noise(0,.01f);
    auto background=silence;
    for(float& v : background) v=noise(rng);
    auto gatedNoise=dsp.render(background,gated);
    assert(rms(gatedNoise,36000)<.05f*rms(background,36000));
    auto a=dsp.render(x,gated), b=dsp.render(x,gated); assert(a==b);
    assert(VoiceSettings::parse("0 20 1 35 800 1").serialize()=="0 20 1 35 800 1");
    assert(VoiceSettings::parse("1 -10 1 500 9000 1").serialize()=="1 0 1 90 1500 1");
    {
        std::mutex mutex; size_t received=0;
        VoiceEffects effects(dir,bypass,[&](const float*,unsigned frames) {
            assert(frames <= 960);
            std::lock_guard<std::mutex> lock(mutex); received+=frames;
        });
        waitFor([&]{return effects.stateJson().find("\"ready\":true")!=std::string::npos;});
        effects.push(x.data(),4096);
        waitFor([&]{std::lock_guard<std::mutex> lock(mutex); return received==4096;});
        effects.record(true);
        for(int i=0;i<8;++i) { effects.push(x.data(),512); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        effects.record(false);
        waitFor([&]{return effects.stateJson().find("\"processed\":true")!=std::string::npos;});
        assert(effects.clipJson(false).find("data:audio/wav;base64,UklGR")!=std::string::npos);
        assert(effects.clipJson(false)==effects.clipJson(true));
        effects.setSettings(low);
        waitFor([&]{return effects.stateJson().find("\"processed\":true")!=std::string::npos;});
        assert(effects.clipJson(false)!=effects.clipJson(true));
        // Bound recordings even if the network sends faster than the inference worker.
        effects.record(true);
        for(int i=0;i<500;++i) effects.push(x.data(),960);
        assert(effects.stateJson().find("\"recording\":false")!=std::string::npos);
        assert(effects.stateJson().find("\"duration\":10.000000")!=std::string::npos);
        waitFor([&]{return effects.stateJson().find("\"processed\":true")!=std::string::npos;});
        effects.record(true); effects.push(x.data(),512); effects.reset();
        assert(effects.stateJson().find("\"recording\":false")!=std::string::npos);
        effects.stop();
        assert(received>0);
    }
    {
        std::atomic<unsigned> received{0};
        VoiceEffects missing(dir+L"/missing",{},[&](const float*,unsigned n) { received+=n; });
        waitFor([&]{return missing.stateJson().find("\"error\":\"\"")==std::string::npos;});
        missing.push(x.data(),120);
        waitFor([&]{return received.load()==120;});
        assert(missing.stateJson().find("\"ready\":false")!=std::string::npos);
    }
    std::cout << "Voice effects: real models, packet boundaries, alignment, rumble, VAD, recording, rerender and fallback passed\n";
}
