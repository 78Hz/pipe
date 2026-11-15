#include "engine/FlueModel.hpp"
#include <cstdlib>
using namespace pipe; using namespace pipe::flue;
// Turbulent jet at the lip, kd = 1 (Verge's drive, no factor), gain held so that a = 2b at u/U = 0.10.
// Reports what the pipe then does: pitch, u/U, swing a/b, build-up time, harmonics.
int main(int argc,char**argv){ int key=atoi(argv[1]); double sign=atof(argv[2]), kd=argc>3?atof(argv[3]):1.0, uT=argc>4?atof(argv[4]):0.10;
 const char* stop=getenv("STOP")?getenv("STOP"):"Octave 8'";
 for (auto& s: opus14::kStops) if (s.div==opus14::kHW && !strcmp(s.name,stop)) { Voicing v; v.ising=familyOf(s)==kString?2.8:2.0; Design d=design(s,key,v);
  JetParams jp; jp.fitted=true; jp.turbulent=true; jp.ddRule=getenv("DD")?atoi(getenv("DD")):0; jp.sign=sign; jp.y0b=0.5; jp.noise=0; jp.kd=kd;
  FlueSim sim; sim.init(d,v,48000);
  jp.gain = 2.0*(sim.b/d.flue)/uT; sim.jp=jp; sim.key(true,kChestPa);
  const double T=4.0; size_t N=T*48000; std::vector<double> vv(N),m(N),t(N); std::vector<double> env; double pk=0;
  for(size_t n=0;n<N;n++){ sim.tickKey(); vv[n]=sim.vPrev; m[n]=sim.mouthOut; t[n]=sim.topOut; pk=std::max(pk,std::fabs(sim.vPrev)); if((n+1)%480==0){env.push_back(pk);pk=0;} }
  size_t from=N-24000,len=24000; double f=peakFreq(vv,from,len,d.target,0.04,48000); double v1=toneAt(vv,from,len,f,48000)*4.0/len, U=sim.Uslot;
  double fin=env.back(); int i10=0,i90=0; while(i10<(int)env.size()&&env[i10]<0.1*fin)i10++; while(i90<(int)env.size()&&env[i90]<0.9*fin)i90++;
  printf("key %d %s sign %+.0f kd %.2f: target %.2f f %.2f (%+.0f c) | W/h %.0f b/h %.1f Vl/U %.2f tau %.2f ms gain %.0f | u/U %.3f a/b %.2f | build 10-90%% %.0f ms\n   H2.. dB:",
    key,stop,sign,kd,d.target,f,1200*log2(f/d.target),d.cutup/d.flue,sim.b/d.flue,sim.lipSpeed,sim.transit/U*1e3,jp.gain,v1/U,(jp.gain*d.flue/U*v1)/sim.b,(i90-i10)*10.0);
  double h1=0; for(int k=1;k<=24&&k*f<20000;k++){ double a=toneAt(m,from,len,k*f,48000),b2=toneAt(t,from,len,k*f,48000),p=sqrt(a*a+b2*b2); if(k==1)h1=p; else printf(" %+.0f",20*log10(p/h1+1e-12)); } printf("\n");
  if (getenv("WAV")) { FlueSim w; w.init(d,v,48000); w.jp=jp; w.jp.noise=getenv("NOISE")?atof(getenv("NOISE")):0.004; std::vector<double> L,R; double pk=0;
    for(size_t n=0;n<(size_t)(6.0*48000);n++){ if(n==(size_t)(0.02*48000)) w.key(true,kChestPa); if(n==(size_t)(3.0*48000)) w.key(false,kChestPa); w.tickKey(); L.push_back(w.mouthOut); R.push_back(w.topOut); pk=std::max(pk,std::max(fabs(w.mouthOut),fabs(w.topOut))); }
    FILE* f=fopen(getenv("WAV"),"wb"); unsigned nn=L.size(), bytes=nn*4; auto w32=[&](unsigned x){fwrite(&x,4,1,f);}; auto w16=[&](unsigned short x){fwrite(&x,2,1,f);};
    fwrite("RIFF",1,4,f); w32(36+bytes); fwrite("WAVEfmt ",1,8,f); w32(16); w16(1); w16(2); w32(48000); w32(48000*4); w16(4); w16(16); fwrite("data",1,4,f); w32(bytes);
    for(size_t i=0;i<nn;i++){ w16((unsigned short)(short)lround(L[i]/pk*0.8*32767)); w16((unsigned short)(short)lround(R[i]/pk*0.8*32767)); } fclose(f); } } }
