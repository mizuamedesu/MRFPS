#include "../../Source/MRFPS/Public/MRFPSConnectionStatus.h"
#include <cassert>

int main()
{
    using namespace MRFPSConnectionStatus;
    FState Status;
    Status.Begin(ERole::Player);
    assert(!Status.ShowSuccess());
    assert(!Status.ObserveJoin(false,true,true,true,false,false,0));
    assert(!Status.ObserveJoin(true,false,true,true,false,false,0));
    assert(!Status.ObserveJoin(true,true,false,true,false,false,0));
    assert(!Status.ObserveJoin(true,true,true,false,false,false,0));
    assert(!Status.ObserveJoin(true,true,true,true,true,true,-1));
    assert(!Status.ObserveJoin(true,true,true,true,false,false,-1));
    assert(Status.ObserveJoin(true,true,true,true,false,false,0));
    Status.AdvanceVisible(30,false,true);
    Status.AdvanceVisible(30,true,false);
    assert(!FocusAllowsVisibility(true,true,false));
    assert(!FocusAllowsVisibility(false,true,true));
    assert(FocusAllowsVisibility(true,true,true));
    assert(FocusAllowsVisibility(true,false,false));
    Status.AdvanceVisible(30,FocusAllowsVisibility(true,true,false),true);
    assert(Status.VisibleSeconds==0);
    Status.AdvanceVisible(30,true,true);
    assert(Status.VisibleSeconds<=.1);
    for (int I=0;I<30;++I) Status.AdvanceVisible(.1,true,true);
    assert(Status.ShowSuccess());
    for (int I=0;I<25;++I) Status.AdvanceVisible(.1,true,true);
    assert(!Status.ShowSuccess());
    Status.Fail(EFailure::Connection);
    assert(!Status.ObserveJoin(true,true,true,true,false,false,0));
    assert(Status.Phase==EPhase::Failure && Status.Failure==EFailure::Connection);
    Status.Begin(ERole::Inspector);
    assert(!Status.ObserveJoin(true,true,true,true,true,true,-1));
    assert(Status.ObserveJoin(true,true,true,true,false,true,-1));
    Status.Begin(ERole::Admin);
    assert(!Status.ObserveJoin(true,true,true,true,false,true,-1));
    assert(Status.ObserveJoin(true,true,true,true,true,true,-1));
    Status.Fail(EFailure::Timeout);
    Status.ClearFailure();
    assert(Status.Phase==EPhase::Idle && Status.Failure==EFailure::None);
}
