#include "audio.h"

#include "config.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <iostream>
#include <unordered_set>

namespace {

/* MinGW's libuuid lacks the WASAPI GUIDs -> define them locally. */
static const GUID k_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4FC2, { 0x9D, 0x42, 0x9C, 0x14, 0x4D, 0x7B, 0x2F, 0x31 } };
static const GUID k_IID_IAudioSessionManager2 =
    { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B, 0x72, 0x0E, 0x36, 0x60, 0x1A, 0x1C, 0x68 } };
static const GUID k_IID_IAudioSessionControl2 =
    { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F, 0xAF, 0xDB, 0xEA, 0x1C, 0x4B, 0x6B, 0xA1 } };
static const GUID k_IID_ISimpleAudioVolume =
    { 0x87CE5498, 0x68D6, 0x44E5, { 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8 } };

bool g_triedInit = false;
IAudioSessionManager2* g_mgr = nullptr;

bool EnsureSessionManager()
{
    if (g_triedInit) return g_mgr != nullptr;
    g_triedInit = true;

    IMMDeviceEnumerator* devEnum = nullptr;
    HRESULT hr = CoCreateInstance(k_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  k_IID_IMMDeviceEnumerator, (void**)&devEnum);
    if (FAILED(hr)) return false;

    IMMDevice* dev = nullptr;
    if (SUCCEEDED(devEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        hr = dev->Activate(k_IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
                           (void**)&g_mgr);
        dev->Release();
    }
    devEnum->Release();
    return SUCCEEDED(hr);
}

} /* namespace */

void AudioApplyBackgroundMute(const std::unordered_set<DWORD>& clientPids,
                              DWORD focusedPid)
{
    if (!config_get_bool("TASX", "MuteBackground", 1)) return;
    if (clientPids.empty()) return;
    if (!EnsureSessionManager() || !g_mgr) return;

    IAudioSessionEnumerator* se = nullptr;
    if (FAILED(g_mgr->GetSessionEnumerator(&se))) return;

    int count = 0;
    se->GetCount(&count);

    std::unordered_set<DWORD> seen;
    for (int i = 0; i < count; ++i)
    {
        IAudioSessionControl* ctl = nullptr;
        if (FAILED(se->GetSession(i, &ctl))) continue;

        IAudioSessionControl2* ctl2 = nullptr;
        if (SUCCEEDED(ctl->QueryInterface(k_IID_IAudioSessionControl2,
                                          (void**)&ctl2)))
        {
            DWORD pid = 0;
            ctl2->GetProcessId(&pid);

            if (pid && clientPids.count(pid))
            {
                seen.insert(pid);
                bool wantMuted = focusedPid != 0 && pid != focusedPid;

                ISimpleAudioVolume* vol = nullptr;
                if (SUCCEEDED(ctl2->QueryInterface(k_IID_ISimpleAudioVolume,
                                                   (void**)&vol)))
                {
                    BOOL mutedNow = FALSE;
                    vol->GetMute(&mutedNow);

                    if (wantMuted && !mutedNow) {
                        if (SUCCEEDED(vol->SetMute(TRUE, nullptr)))
                            std::cout << "[Audio] Background client PID " << pid
                                      << " muted" << std::endl;
                    }
                    else if (!wantMuted && mutedNow) {
                        vol->SetMute(FALSE, nullptr);
                        std::cout << "[Audio] Client PID " << pid
                                  << " unmuted" << std::endl;
                    }
                    vol->Release();
                }
            }
            ctl2->Release();
        }
        ctl->Release();
    }
    se->Release();
}
