#include "../../Source/MRFPS/Public/MRFPSRosterPolicy.h"
#include <cassert>
#include <limits>

int main()
{
    using namespace MRFPSRosterPolicy;
    using R=ERole;
    const auto Viewer=[](bool Local=true, bool Networked=true, bool Encrypted=true, bool Auth=true,
                         bool Inspector=true, bool Bot=false, int Slot=-1, bool Identity=true,
                         std::int64_t Expires=101, std::int64_t Now=100) {
        return CanView(Local,Networked,Encrypted,Auth,Inspector,Bot,Slot,Identity,Expires,Now);
    };
    assert(Viewer());
    assert(!Viewer(false));
    assert(!Viewer(true,false));
    assert(!Viewer(true,true,false));
    assert(!Viewer(true,true,true,false));
    assert(!Viewer(true,true,true,true,false));
    assert(!Viewer(true,true,true,true,true,true));
    assert(!Viewer(true,true,true,true,true,false,0));
    assert(!Viewer(true,true,true,true,true,false,-1,false));
    assert(!Viewer(true,true,true,true,true,false,-1,true,100));

    const auto Role=[](bool Bot,bool Auth,bool Inspector,bool Admin,int Slot,bool Identity=true,
                       std::int64_t Expires=101,std::int64_t Now=100) {
        return Classify(Bot,Auth,Inspector,Admin,Slot,Identity,Expires,Now);
    };
    assert(Role(false,true,false,false,0)==R::Player);
    assert(Role(false,true,false,false,1)==R::Player);
    assert(Role(false,true,true,false,-1)==R::Inspector);
    assert(Role(false,true,true,true,-1)==R::Admin);
    assert(Role(true,false,false,false,1,false,0)==R::Bot);
    assert(Role(false,false,false,false,0)==R::Hidden);
    assert(Role(false,true,false,false,0,false)==R::Hidden);
    assert(Role(false,true,false,false,0,true,100)==R::Hidden);
    assert(Role(false,true,false,false,-1)==R::Hidden);
    assert(Role(false,true,false,false,2)==R::Hidden);
    assert(Role(false,true,false,true,0)==R::Hidden);
    assert(Role(false,true,true,true,0)==R::Hidden);
    assert(Role(true,false,true,false,-1)==R::Hidden);
    assert(Role(true,false,false,false,2)==R::Hidden);

    assert(PingMs(false,0,100,100)==0);
    assert(PingMs(false,42,100,105)==42);
    assert(PingMs(false,60000,100,100)==60000);
    assert(PingMs(true,42,100,100)==-1);
    assert(PingMs(false,-1,100,100)==-1);
    assert(PingMs(false,60001,100,100)==-1);
    assert(PingMs(false,42,100,105.001)==-1);
    assert(PingMs(false,42,100,99.999)==-1);
    assert(PingMs(false,42,-1,100)==-1);
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    assert(PingMs(false,42,nan,100)==-1);
    assert(PingMs(false,42,100,nan)==-1);
    assert(PingMs(false,42,inf,100)==-1);
}
