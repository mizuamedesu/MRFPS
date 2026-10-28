#include "../Source/MRFPSXR/Private/Tests/GeometryMathCases.h"
#include <cstdio>

int main()
{
    int Checked=0, Failed=0;
    RunMRFPSGeometryMathCases([&](bool Passed, const char* Message)
    {
        ++Checked;
        if (!Passed) { ++Failed; std::fprintf(stderr,"FAIL: %s\n",Message); }
    });
    std::printf("Geometry CPU math/topology: %d checks, %d failures. No UE/device/runtime frames.\n",Checked,Failed);
    return Failed==0 ? 0 : 1;
}
