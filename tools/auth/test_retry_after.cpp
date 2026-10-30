#include "../../Source/MRFPS/Private/MRFPSRetryAfter.h"
#include <iostream>
#include <string>

int main()
{
    struct Case { const char* Text; int Expected; };
    const Case Cases[]={
        {"1",1},{"2",2},{"59",59},{"60",60},{"  \t1\t ",1},{"00060",60},
        {"",0},{" ",0},{"0",0},{"00",0},{"61",0},{"100",0},{"999999999999999999",0},
        {"-1",0},{"+1",0},{"1.0",0},{"1e1",0},{"1, 2",0},{"1 2",0},
        {"1\r\n",0},{"1\n",0},{"Mon, 01 Jan 2029 00:00:00 GMT",0},{"1x",0},
        {"\xef\xbc\x91",0},
    };
    int Checked=0;
    for (const auto& Item:Cases) {
        const std::string Input(Item.Text);
        if (MRFPSRetryAfter::Parse(Input.data(),Input.size())!=Item.Expected) return ++Checked;
        ++Checked;
    }
    const std::wstring Wide=L"\t60 ";
    if (MRFPSRetryAfter::Parse(Wide.data(),Wide.size())!=60) return 100;
    const wchar_t NonAscii[]=L"\uff11";
    if (MRFPSRetryAfter::Parse(NonAscii,1)!=0) return 101;
    if (MRFPSRetryAfter::Parse<char>(nullptr,0)!=0) return 102;
    if (MRFPSRetryAfter::Parse(std::string(129,'0').data(),129)!=0) return 103;
    std::cout<<"PASS: Retry-After ASCII bounds, malformed input, whitespace and wide-character cases\n";
    return 0;
}
