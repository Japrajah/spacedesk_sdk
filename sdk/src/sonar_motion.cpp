#include "sonar_controller.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <chrono>

namespace sd::native {
int64_t SonarWallTimeMillis() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
namespace {
int64_t elapsed(int64_t now,int64_t previous) {
    const uint64_t bits=uint64_t(now)-uint64_t(previous);int64_t result;std::memcpy(&result,&bits,sizeof(result));return result;
}
std::array<float,64> coordinates(const SonarMotionPoint& point) {
    if(!point.coordinates_available) throw std::runtime_error("MotionEvent coordinates unavailable");return point.axes;
}
SonarMotionPoint copy_point(const SonarMotionPoint& point,bool catch_coordinates=false) {
    SonarMotionPoint result;result.id=point.id;result.tool=point.tool;
    try { result.axes=coordinates(point); } catch(...) { if(!catch_coordinates) throw; }
    return result;
}
SonarMotionResult unknown_motion(const SonarMotionEvent& event) {
    SonarMotionResult result{event,true};result.event.action=2;result.event.device=0;
    for(size_t i=0;i<event.points.size();++i) {
        result.event.points[i]=copy_point(event.points[i],true);
        result.event.points[i].tool=0;result.event.points[i].axes[2]=0;
    }
    return result; // Original g() always emits MOVE, even after caller set CANCEL.
}
}
SonarMotionResult SonarPenController::Motion(SonarMotionEvent& event,SonarMotionClock& clock) {
    auto& q=state_;
    if(!q.r || q.A || q.B0 || !q.H) return {event,false};
    const size_t count=event.points.size();const int32_t action=event.MaskedAction();
    const auto point=[&](size_t index)->const SonarMotionPoint& { return event.points.at(index); };
    const auto current=[&]()->const SonarMotionPoint& { return point(event.ActionIndex()); };
    if(action==6 || action==1) {
        const int32_t previous=q.D;
        if((action==6 && (previous==current().id || q.D==-1)) || action==1) {
            q.D=-1;q.E=-1;q.F=999;q.v=false;
        }
        if(q.D==-1 && previous!=-1) {
            Status(2);SonarMotionResult result{event,true};
            for(size_t i=0;i<count;++i) {
                result.event.points[i]=copy_point(point(i),true);
                if(point(i).id==previous) { result.event.points[i].tool=2;result.event.points[i].axes[2]=0; }
                else if(q.N && previous!=-1) result.event.points[i].tool=0;
            }
            return result;
        }
        if(q.N) { event.action=3;return unknown_motion(event); }
        return {event,false};
    }
    if(action==0 || action==5 || action==2) {
        q.v=true;
        if(q.k0 && !q.L) {
            if(count>1) {
                const float size=point(0).Size(),pressure=point(0).Pressure();
                float candidate_size=point(0).id==q.E?size:-1,candidate_pressure=point(0).id==q.E?pressure:-1;
                bool sizes_differ=false,pressures_differ=false;
                for(size_t i=1;i<count;++i) {
                    if(point(i).Size()!=size) { if(point(i).id==q.E) candidate_size=point(i).Size();sizes_differ=true; }
                    if(point(i).Pressure()!=pressure) { if(point(i).id==q.E) candidate_pressure=point(i).Pressure();pressures_differ=true; }
                }
                q.L=true;
                if(sizes_differ) { if(q.F==999) q.F=candidate_size;q.M=true; }
                else if(pressures_differ) { if(q.F==999) q.F=candidate_pressure;q.M=true;q.K=false; }
            } else if(q.z0<4) {
                q.x0.at(size_t(q.z0))=point(0).Size();q.y0.at(size_t(q.z0))=point(0).Pressure();++q.z0;
                if(q.z0>=4) {
                    q.L=true;
                    if(q.x0[0]==q.x0[1] && q.x0[1]==q.x0[2] && q.x0[2]==q.x0[3]) {
                        if(q.y0[0]==q.y0[1] && q.y0[1]==q.y0[2] && q.y0[2]==q.y0[3]) q.M=false;
                        else { q.K=false;q.M=true; }
                    } else { q.K=true;q.M=true; }
                }
            }
        }
        UpdatePressure();const float amplitude=q.a0;
        if(q.X==0) q.X=double(amplitude);
        const float metric=q.k0 && q.M?(q.K?current().Size():current().Pressure()):0;
        if(action==0 || action==5) { q.E=current().id;q.J=clock.NowMillis();q.I=0; }
        if(q.D==-1) {
            if(q.R!=3 && (action==0 || action==5)) {
                q.T0=clock.NowMillis();
                while(elapsed(clock.NowMillis(),q.T0)<100) {
                    if(q.B0 || q.A || !q.k || !q.H) break;
                    const uint64_t bits=uint64_t(q.T0)+100;int64_t deadline;std::memcpy(&deadline,&bits,sizeof(deadline));clock.WaitStep(deadline);
                }
                UpdatePressure();q.J=clock.NowMillis();
            }
            const int32_t id=current().id;const auto now=clock.NowMillis();
            if(q.R==3) {
                if(q.D==-1) {
                    if(q.E==id && (action!=2 || elapsed(now,q.J)<175)) {
                        q.n=-1;q.D=id;q.E=-1;
                        if(q.A0 && action==2) event.action=count>1?5:0;
                        if(q.k0 && q.M && metric>0 && q.G==999) q.G=metric;
                        const float initial=action==2?float(q.X):amplitude;
                        if(q.W) { const float threshold=Threshold(initial);if(threshold<q.V) { q.V=threshold;q.w=threshold;q.W=false; } }
                    } else if(action==2 && q.E==id && elapsed(now,q.J)>=175) {
                        event.action=count>1?5:0;q.E=-1;
                    }
                }
            } else if(action==2) {
                const auto age=elapsed(now,q.J);
                if(age<350) {
                    q.I=int32_t(uint32_t(q.I)+1);if(double(q.a0)<q.X) q.X=double(q.a0);
                    if(age>=175 &&q.E==id&&q.A0) { event.action=count>1?5:0;q.E=-1; }
                } else if(q.k0 && q.M && q.G!=999 && metric!=0) {
                    if((double(q.G)>.01 && metric>q.G*1.75f) || (double(q.G)<.01 && metric>q.G*5)) {
                        q.E=-1;if(q.N) { event.action=3;return unknown_motion(event); }return {event,false};
                    }
                }
            }
        }
    }
    if((action==0 || action==5) && q.D==-1 && q.A0) {
        if(action==0 || q.N) { if(q.N) event.action=3;return unknown_motion(event); }
        SonarMotionResult result{event,true};result.event.points.clear();result.event.action=2;result.event.device=0;
        for(size_t i=0;i<count;++i) {
            auto copy=copy_point(point(i));if(i!=event.ActionIndex()) result.event.points.push_back(std::move(copy));
        }
        return result;
    }
    float pressure=q.m;
    if(q.n>0) {
        float difference=q.n-pressure;
        if(q.q==0) difference/=2;
        else if(q.q==1) difference=(difference*2)/3;
        else difference=(difference*3)/4;
        pressure+=difference;if(pressure<0) pressure=0;if(pressure>1) pressure=1;
    }
    if(q.D!=-1) q.n=pressure;
    SonarMotionResult result{event,true};result.event.points.clear();bool sonar=false;
    SonarMotionPoint first,selected;
    for(size_t i=0;i<count;++i) {
        auto copy=copy_point(point(i));
        if(q.R==3 && q.v && copy.tool==1) {
            if(q.D==copy.id) {
                if(q.k0 &&q.M) { const auto value=q.K?point(i).Size():point(i).Pressure();if(q.G>value) q.G=value; }
                copy.tool=2;copy.axes[2]=std::max(pressure,.011f);if(q.o) copy.axes[3]=q.p;sonar=true;
            } else if(q.N) { copy.tool=0;copy.axes[2]=0;copy.axes[3]=0; }
        }
        if(i==0) first=copy;
        if(q.N) { if(copy.tool==2) selected=copy; }
        else result.event.points.push_back(std::move(copy));
    }
    if(q.N) {
        if(!sonar) { selected=first;event.action=3; }
        result.event.points.push_back(std::move(selected));
    }
    result.event.action=event.action;return result;
}
}
