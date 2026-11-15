#include "engine/FlueModel.hpp"
#include <cstdlib>
using namespace pipe; using namespace pipe::flue;
// Energy budget at the fundamental: for each pressure term p_i, the power it delivers to the
// pipe is <p_i q> (q = acoustic volume flow into the mouth). Also geometry and passive admittance.
int main(int argc,char**argv){ int key=atoi(argv[1]); double kd=atof(argv[2]);
 for (auto& s: opus14::kStops) if (s.div==opus14::kHW && !strcmp(s.name,"Octave 8'")) { Voicing v; v.ising=2.0; Design d=design(s,key,v);
  JetParams jp; jp.fitted=true; jp.sign=-1; jp.gamma=0.4; jp.y0b=0.5; jp.gain=8; jp.noise=0; jp.kd=kd;
  FlueSim sim; sim.init(d,v,48000); sim.jp=jp; sim.key(true,kChestPa);
  const int N=48000*4, A=48000*3; double Pd=0,Pv=0,Pvol=0,qq=0,dd2=0,vv2=0,vol2=0,eta2=0,v2=0;
  for(int n=0;n<N;n++){ sim.tickKey(); if(n>=A){ double q=sim.vPrev*sim.Sm; Pd+=sim.dbgDrive*q; Pv+=sim.dbgVortex*q; Pvol+=sim.dbgVol*q; qq+=q*q;
     dd2+=sim.dbgDrive*sim.dbgDrive; vv2+=sim.dbgVortex*sim.dbgVortex; vol2+=sim.dbgVol*sim.dbgVol; eta2+=sim.dbgEta*sim.dbgEta; v2+=sim.vPrev*sim.vPrev; } }
  const int M=N-A; auto r=[&](double x){return std::sqrt(x/M);};
  const double U=sim.Uslot, h=d.flue, W=d.cutup;
  cd Y=flueAdmittance(2*M_PI*d.modes[0].f,d.g,Air{});
  printf("key %d f1 %.1f Hz Q1 %.0f | h %.3f mm W %.2f mm Wm %.1f mm W/h %.1f dd %.2f mm Sm %.1f mm2 | U %.1f m/s |Y1| %.3g m3/(s Pa)\n",
    key,d.modes[0].f,d.modes[0].Q,h*1e3,W*1e3,d.mouthWidth*1e3,W/h,sim.dd*1e3,sim.Sm*1e6,U,std::abs(Y));
  printf("  kd %.2f: v_rms %.2f m/s (u/U %.3f) eta_rms/b %.2f | rms Pa: drive %.1f vortex %.1f vol %.2f | power mW: drive %+.3f vortex %+.3f vol %+.4f (into pipe +)\n",
    kd,r(v2),r(v2)*1.414/U,r(eta2)/(0.4*h),r(dd2),r(vv2),r(vol2),Pd/M*1e3,Pv/M*1e3,Pvol/M*1e3);
  // passive loss: power lost in the resonator = <dp q> total = q_rms^2 / Re(Y)... report Re(1/Y)
  printf("  pipe input resistance at f1: Re(1/Y) %.3g Pa s/m3 -> passive loss at this q: %.3f mW\n", std::real(1.0/Y), std::real(1.0/Y)*qq/M*1e3); } }
