#include "MoBitsDiscovery.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"
#if PLATFORM_ANDROID && USE_ANDROID_JNI
#include "Android/AndroidJavaEnv.h"

namespace
{
FString AndroidInternalFilesDirectory()
{
    // UE5.8 exports these through Core's public AndroidJavaEnv API. Resolve
    // Context.getFilesDir(), not GamePersistentDownloadDir (external storage).
    JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
    jobject Activity = AndroidJavaEnv::GetGameActivityThis();
    if (!Env || !Activity) return FString();
    if (Env->PushLocalFrame(8) != JNI_OK)
    {
        if (Env->ExceptionCheck()) Env->ExceptionClear();
        return FString();
    }
    struct FLocalFrame
    {
        JNIEnv* Env;
        ~FLocalFrame() { Env->PopLocalFrame(nullptr); }
    } Frame{Env};
    const auto Failed = [Env](bool bInvalid)
    {
        if (Env->ExceptionCheck()) { Env->ExceptionClear(); return true; }
        return bInvalid;
    };
    jclass ActivityClass = Env->GetObjectClass(Activity);
    if (Failed(!ActivityClass)) return FString();
    jmethodID GetFilesDir = Env->GetMethodID(ActivityClass, "getFilesDir", "()Ljava/io/File;");
    if (Failed(!GetFilesDir)) return FString();
    jobject Directory = Env->CallObjectMethod(Activity, GetFilesDir);
    if (Failed(!Directory)) return FString();
    jclass FileClass = Env->GetObjectClass(Directory);
    if (Failed(!FileClass)) return FString();
    jmethodID GetAbsolutePath = Env->GetMethodID(FileClass, "getAbsolutePath", "()Ljava/lang/String;");
    if (Failed(!GetAbsolutePath)) return FString();
    jstring Path = static_cast<jstring>(Env->CallObjectMethod(Directory, GetAbsolutePath));
    if (Failed(!Path)) return FString();
    FString Result = FJavaHelper::FStringFromLocalRef(Env, Path);
    if (Failed(Result.IsEmpty())) return FString();
    return Result;
}
}
#endif

FString MoBitsDiscovery::ConnectionFilePath()
{
    FString Root;
#if PLATFORM_WINDOWS
    Root = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
#elif PLATFORM_MAC
    Root = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
    if (!Root.IsEmpty()) Root = FPaths::Combine(Root, TEXT("Library/Application Support"));
#elif PLATFORM_LINUX
    Root = FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_CONFIG_HOME"));
    if (Root.IsEmpty() || FPaths::IsRelative(Root))
    {
        Root = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
        if (!Root.IsEmpty()) Root = FPaths::Combine(Root, TEXT(".config"));
    }
#elif PLATFORM_ANDROID
    // MRFPS extension: adb reverse preserves the SDK's authenticated loopback
    // transport. quest_bridge.py mirrors the current desktop discovery file.
    // Credentials stay in internal app-private files, provisioned as the app UID.
#if USE_ANDROID_JNI
    Root = AndroidInternalFilesDirectory();
#endif
#endif
    if (Root.IsEmpty() || FPaths::IsRelative(Root)) return FString();
    return FPaths::Combine(Root, TEXT("MoBits/SDK/connection-v2.json"));
}
bool MoBitsDiscovery::Read(FString& Json, FString& Error) { return ReadFile(ConnectionFilePath(), Json, Error); }
bool MoBitsDiscovery::ReadFile(const FString& Path, FString& Json, FString& Error)
{
    Json.Empty(); Error.Empty();
    TUniquePtr<FArchive> File(Path.IsEmpty() ? nullptr : IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if (!File)
    { Error = TEXT("MoBits app was not found. Launch MoBits on this PC and enable Unity / UE5 SDK if closed. No copying is required."); return false; }
    const int64 Size = File->TotalSize();
    if (Size <= 0 || Size > 4096)
    { Json.Empty(); Error = TEXT("Cannot read MoBits local connection information. Restart MoBits under the same OS user."); return false; }
    TArray<uint8> Bytes; Bytes.SetNumZeroed(static_cast<int32>(Size) + 2);
    File->Serialize(Bytes.GetData(),Size);
    if (File->IsError()) { Error = TEXT("Cannot read MoBits local connection information. Retry Connect."); return false; }
    FFileHelper::BufferToString(Json,Bytes.GetData(),static_cast<int32>(Size));
    return true;
}
