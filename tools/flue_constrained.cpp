// Jet held to measured organ-pipe behaviour: swing a = 2b (Fletcher & Douglas 1980) and mouth
// velocity u/U = target (Hirschberg et al. 1995). G = 2b U/(h v) = 0.8/(u/U); kd iterated.
#include "engine/FlueModel.hpp"
#include <cstdlib>
using namespace pipe; using namespace pipe::flue;
struct Out { double uU, aB, f; std::vector<double> m, t; };
static Out run(const Design& d, const Voicing& v, JetParams jp) {
  FlueSim sim; sim.init(d,v,48000); sim.jp=jp; sim.key(true,kChestPa);
  const double T=settleSeconds(d)+0.6; size_t N=T*48000; Out o; o.m.resize(N); o.t.resize(N); std::vector<double> vv(N);
  for(size_t n=0;n<N;n++){ sim.tickKey(); o.m[n]=sim.mouthOut; o.t[n]=sim.topOut; vv[n]=sim.vPrev; }
  size_t from=N-24000, len=24000; o.f=peakFreq(vv,from,len,d.target,0.04,48000);
  // amplitude of the fundamental of v: Hann DFT magnitude * 4/len
  double v1=toneAt(vv,from,len,o.f,48000)*4.0/len; double U=sim.Uslot;
  o.uU=v1/U; o.aB=(jp.gain*d.flue/U*v1)/(0.4*d.flue); return o;
}
int main(int argc,char**argv){
  double uTarget=argc>1?atof(argv[1]):0.15, y0=argc>2?atof(argv[2]):1.0; int key=argc>3?atoi(argv[3]):43;
  const char* stop=argc>4?argv[4]:"Octave 8'"; int div=argc>5?atoi(argv[5]):0;
  for (auto& s: opus14::kStops) if (s.div==(opus14::Division)div && !strcmp(s.name,stop)) {
    Voicing v; v.ising=familyOf(s)==kString?2.8:2.0; Design d=design(s,key,v);
    JetParams jp; jp.fitted=true; jp.sign=getenv("SIGN")?atof(getenv("SIGN")):1; jp.gamma=getenv("GAMMA")?atof(getenv("GAMMA")):0.4; jp.y0b=y0; jp.noise=0; jp.gain=0.8/uTarget; jp.kd=1.0;
    Out o; double bestErr=1e9, bestKd=1;
    for(double kd=4; kd<=128; kd*=1.19){ jp.kd=kd; Out t=run(d,v,jp); double e=fabs(log(std::max(1e-6,t.uU)/uTarget)); if(e<bestErr){bestErr=e;bestKd=kd;} }
    jp.kd=bestKd; o=run(d,v,jp);
    fprintf(stderr,"%s key %d: gain %.2f kd %.3f -> u/U %.3f, swing %.2f b, f %.2f\n", stop,key,jp.gain,jp.kd,o.uU,o.aB,o.f);
    // full spectrum, power sum of mouth and top
    size_t N=o.m.size(), from=N-24000, len=24000; double h1=0;
    printf("%s key %d u/U %.3f a/b %.2f y0/b %.2f :", stop,key,o.uU,o.aB,y0);
    for(int k=1;k<=24 && k*o.f<20000;k++){ double a=toneAt(o.m,from,len,k*o.f,48000), b=toneAt(o.t,from,len,k*o.f,48000), p=sqrt(a*a+b*b); if(k==1) h1=p; else printf(" %+.0f",20*log10(p/h1)); }
    printf("\n");
    // write stereo wav for listening
    if (argc>6) { FILE* f=fopen(argv[6],"wb"); FlueSim sim; sim.init(d,v,48000); sim.jp=jp; sim.jp.noise=0.004; std::vector<short> buf; double pk=0; std::vector<double> L,R;
      for(size_t n=0;n<(size_t)(4.6*48000);n++){ if(n==(size_t)(0.02*48000)) sim.key(true,kChestPa); if(n==(size_t)(2.6*48000)) sim.key(false,kChestPa); sim.tickKey(); L.push_back(sim.mouthOut); R.push_back(sim.topOut); pk=std::max(pk,std::max(fabs(sim.mouthOut),fabs(sim.topOut))); }
      unsigned nn=L.size(), bytes=nn*4; auto w32=[&](unsigned x){fwrite(&x,4,1,f);}; auto w16=[&](unsigned short x){fwrite(&x,2,1,f);};
      fwrite("RIFF",1,4,f); w32(36+bytes); fwrite("WAVEfmt ",1,8,f); w32(16); w16(1); w16(2); w32(48000); w32(48000*4); w16(4); w16(16); fwrite("data",1,4,f); w32(bytes);
      for(size_t i=0;i<nn;i++){ w16((unsigned short)(short)lround(L[i]/pk*0.8*32767)); w16((unsigned short)(short)lround(R[i]/pk*0.8*32767)); } fclose(f); }
  } }
