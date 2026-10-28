#include "../Source/MRFPSAuth/Private/MRFPSAuthCrypto.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
bool Read(const char* Path, std::size_t Limit, std::string& Out)
{
    std::ifstream Input(Path, std::ios::binary | std::ios::ate);
    if (!Input || Input.tellg() <= 0 || Input.tellg() > static_cast<std::streamoff>(Limit)) return false;
    Input.seekg(0);
    Out.assign(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
    return Input.good() || Input.eof();
}
}

int main(int Count, char** Args)
{
    // File paths and public pin only; never accept a bearer token argument.
    if (Count != 7) { std::cerr << "Expected public-key file, token file, audience, role, public DTLS fingerprint, UTC seconds.\n"; return 2; }
    std::string Pem, Token;
    if (!Read(Args[1], 1024, Pem) || !Read(Args[2], 4096, Token)) { std::cerr << "AUTH_FIXTURE_FILE\n"; return 1; }
    const auto First = Token.find_first_not_of(" \t\r\n");
    const auto Last = Token.find_last_not_of(" \t\r\n");
    Token = First == std::string::npos ? std::string() : Token.substr(First, Last - First + 1);
    MRFPSAuthCore::PublicKey Key;
    MRFPSAuthCore::Claims Claims;
    std::string SafeError;
    std::int64_t Now = 0;
    try { Now = std::stoll(Args[6]); }
    catch (...) { std::cerr << "AUTH_FIXTURE_CLOCK\n"; return 1; }
    if (!Key.LoadPem(Pem) || !MRFPSAuthCore::Verify(Token, Key, Args[3], Args[4], Now, Claims, SafeError, Args[5]))
    { std::cerr << (SafeError.empty() ? "AUTH_CONFIG" : SafeError) << '\n'; return 1; }
    std::cout << "MRFPS_AUTH_INTEROP_OK: independent issuer signature, claims, and DTLS binding verified.\n";
    return 0;
}
