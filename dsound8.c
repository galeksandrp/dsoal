/* DirectSound COM interface
 *
 * Copyright 2009 Maarten Lankhorst
 *
 * Some code taken from the original dsound-openal implementation
 *    Copyright 2007-2009 Chris Robinson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <string.h>

#include <windows.h>
#include <dsound.h>

#include "dsound_private.h"

#ifndef DSSPEAKER_7POINT1
#define DSSPEAKER_7POINT1       7
#endif


static DeviceShare **sharelist;
static UINT sharelistsize;

static void DSShare_Destroy(DeviceShare *share)
{
    UINT i;

    EnterCriticalSection(&openal_crst);
    for(i = 0;i < sharelistsize;i++)
    {
        if(sharelist[i] == share)
        {
            sharelist[i] = sharelist[--sharelistsize];
            if(sharelistsize == 0)
            {
                HeapFree(GetProcessHeap(), 0, sharelist);
                sharelist = NULL;
            }
            break;
        }
    }
    LeaveCriticalSection(&openal_crst);

    if(share->ctx)
    {
        /* Calling setALContext is not appropriate here,
         * since we *have* to unset the context before destroying it
         */
        ALCcontext *old_ctx;

        EnterCriticalSection(&openal_crst);
        old_ctx = get_context();
        if(old_ctx != share->ctx)
            set_context(share->ctx);
        else
            old_ctx = NULL;

        if(share->nsources)
            alDeleteSources(share->nsources, share->sources);

        if(share->auxslot)
            share->ExtAL.DeleteAuxiliaryEffectSlots(1, &share->auxslot);

        set_context(old_ctx);
        alcDestroyContext(share->ctx);
        LeaveCriticalSection(&openal_crst);
    }

    if(share->device)
        alcCloseDevice(share->device);
    share->device = NULL;

    DeleteCriticalSection(&share->crst);

    HeapFree(GetProcessHeap(), 0, share);
}

static HRESULT DSShare_Create(REFIID guid, DeviceShare **out)
{
    const ALCchar *drv_name;
    DeviceShare *share;
    void *temp;
    HRESULT hr;
    DWORD n;

    share = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*share));
    if(!share) return DSERR_OUTOFMEMORY;
    share->ref = 1;

    InitializeCriticalSection(&share->crst);

    hr = DSERR_NODRIVER;
    if(!(drv_name=DSOUND_getdevicestrings()) ||
       memcmp(guid, &DSOUND_renderer_guid, sizeof(GUID)-1) != 0)
    {
        WARN("No device found\n");
        goto fail;
    }

    n = guid->Data4[7];
    while(*drv_name && n--)
        drv_name += strlen(drv_name) + 1;
    if(!*drv_name)
    {
        WARN("No device string found\n");
        goto fail;
    }
    memcpy(&share->guid, guid, sizeof(GUID));

    share->device = alcOpenDevice(drv_name);
    if(!share->device)
    {
        alcGetError(NULL);
        WARN("Couldn't open device \"%s\"\n", drv_name);
        goto fail;
    }
    TRACE("Opened device: %s\n", alcGetString(share->device, ALC_DEVICE_SPECIFIER));

    hr = DSERR_NODRIVER;
    share->ctx = alcCreateContext(share->device, NULL);
    if(!share->ctx)
    {
        ALCenum err = alcGetError(share->device);
        ERR("Could not create context (0x%x)!\n", err);
        goto fail;
    }

    setALContext(share->ctx);
    if(alIsExtensionPresent("AL_EXT_FLOAT32"))
    {
        TRACE("Found AL_EXT_FLOAT32\n");
        share->SupportedExt[EXT_FLOAT32] = AL_TRUE;
    }
    if(alIsExtensionPresent("AL_EXT_MCFORMATS"))
    {
        TRACE("Found AL_EXT_MCFORMATS\n");
        share->SupportedExt[EXT_MCFORMATS] = AL_TRUE;
    }
    if(alIsExtensionPresent("AL_SOFT_deferred_updates"))
    {
        TRACE("Found AL_SOFT_deferred_updates\n");
        share->ExtAL.DeferUpdatesSOFT = alGetProcAddress("alDeferUpdatesSOFT");
        share->ExtAL.ProcessUpdatesSOFT = alGetProcAddress("alProcessUpdatesSOFT");
        share->SupportedExt[SOFT_DEFERRED_UPDATES] = AL_TRUE;
    }

    if(alcIsExtensionPresent(share->device, "ALC_EXT_EFX"))
    {
#define LOAD_FUNC(x) (share->ExtAL.x = alGetProcAddress("al"#x))
        LOAD_FUNC(GenEffects);
        LOAD_FUNC(DeleteEffects);
        LOAD_FUNC(Effecti);
        LOAD_FUNC(Effectf);

        LOAD_FUNC(GenAuxiliaryEffectSlots);
        LOAD_FUNC(DeleteAuxiliaryEffectSlots);
        LOAD_FUNC(AuxiliaryEffectSloti);
#undef LOAD_FUNC
        share->SupportedExt[EXT_EFX] = AL_TRUE;

        share->ExtAL.GenAuxiliaryEffectSlots(1, &share->auxslot);
    }

    share->max_sources = 0;
    while(share->max_sources < MAX_SOURCES)
    {
        alGenSources(1, &share->sources[share->max_sources]);
        if(alGetError() != AL_NO_ERROR)
            break;
        share->max_sources++;
    }
    share->nsources = share->max_sources;
    popALContext();

    if(sharelist)
        temp = HeapReAlloc(GetProcessHeap(), 0, sharelist, sizeof(*sharelist)*(sharelistsize+1));
    else
        temp = HeapAlloc(GetProcessHeap(), 0, sizeof(*sharelist)*(sharelistsize+1));
    if(temp)
    {
        sharelist = temp;
        sharelist[sharelistsize++] = share;
    }

    *out = share;
    return DS_OK;

fail:
    DSShare_Destroy(share);
    return hr;
}

static ULONG DSShare_AddRef(DeviceShare *share)
{
    ULONG ref = InterlockedIncrement(&share->ref);
    return ref;
}

static ULONG DSShare_Release(DeviceShare *share)
{
    ULONG ref = InterlockedDecrement(&share->ref);
    if(ref == 0) DSShare_Destroy(share);
    return ref;
}


static const IDirectSound8Vtbl DS8_Vtbl;
static const IDirectSoundVtbl DS_Vtbl;

static inline DS8Impl *impl_from_IDirectSound8(IDirectSound8 *iface)
{
    return CONTAINING_RECORD(iface, DS8Impl, IDirectSound8_iface);
}

static inline DS8Impl *impl_from_IDirectSound(IDirectSound *iface)
{
    return CONTAINING_RECORD(iface, DS8Impl, IDirectSound_iface);
}

HRESULT DSOUND_Create(REFIID riid, void **ds)
{
    HRESULT hr;

    hr = DSOUND_Create8(&IID_IDirectSound, ds);
    if(SUCCEEDED(hr))
    {
        DS8Impl *impl = impl_from_IDirectSound(*ds);
        impl->is_8 = FALSE;

        if(!IsEqualIID(riid, &IID_IDirectSound))
        {
            hr = IDirectSound_QueryInterface(&impl->IDirectSound_iface, riid, ds);
            IDirectSound_Release(&impl->IDirectSound_iface);
        }
    }
    return hr;
}

static void DS8Impl_Destroy(DS8Impl *This);

static const WCHAR speakerconfigkey[] = {
    'S','Y','S','T','E','M','\\',
    'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
    'C','o','n','t','r','o','l','\\',
    'M','e','d','i','a','R','e','s','o','u','r','c','e','s','\\',
    'D','i','r','e','c','t','S','o','u','n','d','\\',
    'S','p','e','a','k','e','r',' ','C','o','n','f','i','g','u','r','a','t','i','o','n',0
};

static const WCHAR speakerconfig[] = {
    'S','p','e','a','k','e','r',' ','C','o','n','f','i','g','u','r','a','t','i','o','n',0
};

HRESULT DSOUND_Create8(REFIID riid, LPVOID *ds)
{
    DS8Impl *This;
    HKEY regkey;
    HRESULT hr;

    *ds = NULL;
    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if(!This) return DSERR_OUTOFMEMORY;

    This->IDirectSound8_iface.lpVtbl = (IDirectSound8Vtbl*)&DS8_Vtbl;
    This->IDirectSound_iface.lpVtbl = (IDirectSoundVtbl*)&DS_Vtbl;

    This->is_8 = TRUE;
    This->speaker_config = DSSPEAKER_COMBINED(DSSPEAKER_5POINT1, DSSPEAKER_GEOMETRY_WIDE);

    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, speakerconfigkey, 0, KEY_READ, &regkey) == ERROR_SUCCESS)
    {
        DWORD type, conf, confsize = sizeof(DWORD);

        if(RegQueryValueExW(regkey, speakerconfig, NULL, &type, (BYTE*)&conf, &confsize) == ERROR_SUCCESS)
        {
            if(type == REG_DWORD)
                This->speaker_config = conf;
        }

        RegCloseKey(regkey);
    }
    /*RegGetValueW(HKEY_LOCAL_MACHINE, speakerconfigkey, speakerconfig, RRF_RT_REG_DWORD, NULL, &This->speaker_config, NULL);*/

    hr = IDirectSound8_QueryInterface(&This->IDirectSound8_iface, riid, ds);
    if(FAILED(hr))
        DS8Impl_Destroy(This);
    return hr;
}

static void DS8Impl_Destroy(DS8Impl *This)
{
    DS8Primary_Clear(&This->primary);
    if(This->share)
        DSShare_Release(This->share);
    This->share = NULL;

    HeapFree(GetProcessHeap(), 0, This);
}


static HRESULT WINAPI DS8_QueryInterface(IDirectSound8 *iface, REFIID riid, LPVOID *ppv)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);

    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    *ppv = NULL;
    if(IsEqualIID(riid, &IID_IUnknown))
        *ppv = &This->IDirectSound8_iface;
    else if(IsEqualIID(riid, &IID_IDirectSound8))
    {
        if(This->is_8)
            *ppv = &This->IDirectSound8_iface;
    }
    else if(IsEqualIID(riid, &IID_IDirectSound))
        *ppv = &This->IDirectSound_iface;
    else
        FIXME("Unhandled GUID: %s\n", debugstr_guid(riid));

    if(*ppv)
    {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI DS8_AddRef(IDirectSound8 *iface)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    LONG ref;

    ref = InterlockedIncrement(&This->ref);
    TRACE("Reference count incremented to %ld\n", ref);

    return ref;
}

static ULONG WINAPI DS8_Release(IDirectSound8 *iface)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    LONG ref;

    ref = InterlockedDecrement(&This->ref);
    TRACE("Reference count decremented to %ld\n", ref);
    if(ref == 0)
        DS8Impl_Destroy(This);

    return ref;
}

static HRESULT WINAPI DS8_CreateSoundBuffer(IDirectSound8 *iface, LPCDSBUFFERDESC desc, LPLPDIRECTSOUNDBUFFER buf, IUnknown *pUnkOuter)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    HRESULT hr;

    TRACE("(%p)->(%p, %p, %p)\n", iface, desc, buf, pUnkOuter);

    if(!buf)
    {
        WARN("buf is null\n");
        return DSERR_INVALIDPARAM;
    }
    *buf = NULL;

    if(pUnkOuter)
    {
        WARN("Aggregation isn't supported\n");
        return DSERR_NOAGGREGATION;
    }
    if(!desc || desc->dwSize < sizeof(DSBUFFERDESC1))
    {
        WARN("Invalid buffer %p/%lu\n", desc, desc?desc->dwSize:0);
        return DSERR_INVALIDPARAM;
    }

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    TRACE("Requested buffer:\n"
          "    Size        = %lu\n"
          "    Flags       = 0x%08lx\n"
          "    BufferBytes = %lu\n",
          desc->dwSize, desc->dwFlags, desc->dwBufferBytes);

    if(desc->dwSize >= sizeof(DSBUFFERDESC))
    {
        if(!(desc->dwFlags&DSBCAPS_CTRL3D))
        {
            if(!IsEqualGUID(&desc->guid3DAlgorithm, &GUID_NULL))
            {
                WARN("Invalid 3D algorithm GUID specified for non-3D buffer: %s\n", debugstr_guid(&desc->guid3DAlgorithm));
                return DSERR_INVALIDPARAM;
            }
        }
        else
            TRACE("Requested 3D algorithm GUID: %s\n", debugstr_guid(&desc->guid3DAlgorithm));
    }

    /* OpenAL doesn't support playing with 3d and panning at same time.. */
    if((desc->dwFlags&(DSBCAPS_CTRL3D|DSBCAPS_CTRLPAN)) == (DSBCAPS_CTRL3D|DSBCAPS_CTRLPAN))
    {
        if(!This->is_8)
            ERR("Cannot create buffers with 3D and panning control\n");
        else
            WARN("Cannot create buffers with 3D and panning control\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->primary.crst);
    if((desc->dwFlags&DSBCAPS_PRIMARYBUFFER))
    {
        IDirectSoundBuffer *prim = &This->primary.IDirectSoundBuffer_iface;

        hr = S_OK;
        if(IDirectSoundBuffer_AddRef(prim) == 1)
        {
            hr = IDirectSoundBuffer_Initialize(prim, &This->IDirectSound_iface, desc);
            if(FAILED(hr))
            {
                IDirectSoundBuffer_Release(prim);
                prim = NULL;
            }
        }
        *buf = prim;
    }
    else
    {
        DS8Buffer *dsb;

        hr = DS8Buffer_Create(&dsb, &This->primary, NULL);
        if(SUCCEEDED(hr))
        {
            hr = IDirectSoundBuffer8_Initialize(&dsb->IDirectSoundBuffer8_iface, &This->IDirectSound_iface, desc);
            if(FAILED(hr))
                IDirectSoundBuffer8_Release(&dsb->IDirectSoundBuffer8_iface);
            else
            {
                dsb->bufferlost = (This->prio_level == DSSCL_WRITEPRIMARY);
                *buf = &dsb->IDirectSoundBuffer_iface;
            }
        }
    }
    LeaveCriticalSection(This->primary.crst);

    TRACE("%08lx\n", hr);
    return hr;
}

static HRESULT WINAPI DS8_GetCaps(IDirectSound8 *iface, LPDSCAPS caps)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);

    TRACE("(%p)->(%p)\n", iface, caps);

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    if(!caps || caps->dwSize < sizeof(*caps))
    {
        WARN("Invalid DSCAPS (%p, %lu)\n", caps, (caps?caps->dwSize:0));
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(&This->share->crst);

    caps->dwFlags = DSCAPS_CONTINUOUSRATE |
                    DSCAPS_PRIMARY16BIT | DSCAPS_PRIMARYSTEREO |
                    DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARYMONO |
                    DSCAPS_SECONDARY16BIT | DSCAPS_SECONDARY8BIT |
                    DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO;
    caps->dwPrimaryBuffers = 1;
    caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    caps->dwMaxHwMixingAllBuffers =
        caps->dwMaxHwMixingStaticBuffers =
        caps->dwMaxHwMixingStreamingBuffers =
        caps->dwMaxHw3DAllBuffers =
        caps->dwMaxHw3DStaticBuffers =
        caps->dwMaxHw3DStreamingBuffers = This->share->max_sources;
    caps->dwFreeHwMixingAllBuffers =
        caps->dwFreeHwMixingStaticBuffers =
        caps->dwFreeHwMixingStreamingBuffers =
        caps->dwFreeHw3DAllBuffers =
        caps->dwFreeHw3DStaticBuffers =
        caps->dwFreeHw3DStreamingBuffers = This->share->nsources;
    caps->dwTotalHwMemBytes =
        caps->dwFreeHwMemBytes = 64 * 1024 * 1024;
    caps->dwMaxContigFreeHwMemBytes = caps->dwFreeHwMemBytes;
    caps->dwUnlockTransferRateHwBuffers = 4096;
    caps->dwPlayCpuOverheadSwBuffers = 0;

    LeaveCriticalSection(&This->share->crst);

    return DS_OK;
}
static HRESULT WINAPI DS8_DuplicateSoundBuffer(IDirectSound8 *iface, IDirectSoundBuffer *in, IDirectSoundBuffer **out)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    DS8Buffer *buf;
    DSBCAPS caps;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", iface, in, out);

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    if(!in || !out)
    {
        WARN("Invalid pointer: int = %p, out = %p\n", in, out);
        return DSERR_INVALIDPARAM;
    }
    *out = NULL;

    caps.dwSize = sizeof(caps);
    hr = IDirectSoundBuffer_GetCaps(in, &caps);
    if(SUCCEEDED(hr) && (caps.dwFlags&DSBCAPS_PRIMARYBUFFER))
    {
        WARN("Cannot duplicate buffer %p, which has DSBCAPS_PRIMARYBUFFER\n", in);
        hr = DSERR_INVALIDPARAM;
    }
    if(SUCCEEDED(hr) && (caps.dwFlags&DSBCAPS_CTRLFX))
    {
        WARN("Cannot duplicate buffer %p, which has DSBCAPS_CTRLFX\n", in);
        hr = DSERR_INVALIDPARAM;
    }
    if(SUCCEEDED(hr))
        hr = DS8Buffer_Create(&buf, &This->primary, in);
    if(SUCCEEDED(hr))
    {
        *out = &buf->IDirectSoundBuffer_iface;
        hr = IDirectSoundBuffer_Initialize(*out, NULL, NULL);
    }
    if(SUCCEEDED(hr))
    {
        /* According to MSDN volume isn't copied */
        if((caps.dwFlags&DSBCAPS_CTRLPAN))
        {
            LONG pan;
            if(SUCCEEDED(IDirectSoundBuffer_GetPan(in, &pan)))
                IDirectSoundBuffer_SetPan(*out, pan);
        }
        if((caps.dwFlags&DSBCAPS_CTRLFREQUENCY))
        {
            DWORD freq;
            if(SUCCEEDED(IDirectSoundBuffer_GetFrequency(in, &freq)))
                IDirectSoundBuffer_SetFrequency(*out, freq);
        }
        if((caps.dwFlags&DSBCAPS_CTRL3D))
        {
            IDirectSound3DBuffer *buf3d;
            DS3DBUFFER DS3DBuffer;
            HRESULT subhr;

            subhr = IDirectSound_QueryInterface(in, &IID_IDirectSound3DBuffer, (void**)&buf3d);
            if(SUCCEEDED(subhr))
            {
                DS3DBuffer.dwSize = sizeof(DS3DBuffer);
                subhr = IDirectSound3DBuffer_GetAllParameters(buf3d, &DS3DBuffer);
                IDirectSound3DBuffer_Release(buf3d);
            }
            if(SUCCEEDED(subhr))
                subhr = IDirectSoundBuffer_QueryInterface(*out, &IID_IDirectSound3DBuffer, (void**)&buf3d);
            if(SUCCEEDED(subhr))
            {
                subhr = IDirectSound3DBuffer_SetAllParameters(buf3d, &DS3DBuffer, DS3D_IMMEDIATE);
                IDirectSound3DBuffer_Release(buf3d);
            }
        }
    }
    if(FAILED(hr))
    {
        if(*out)
            IDirectSoundBuffer_Release(*out);
        *out = NULL;
    }

    return hr;
}

static HRESULT WINAPI DS8_SetCooperativeLevel(IDirectSound8 *iface, HWND hwnd, DWORD level)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->(%p, %lu)\n", iface, hwnd, level);

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    if(level > DSSCL_WRITEPRIMARY || level < DSSCL_NORMAL)
    {
        WARN("Invalid coop level: %lu\n", level);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->primary.crst);
    if(level == DSSCL_WRITEPRIMARY && (This->prio_level != DSSCL_WRITEPRIMARY))
    {
        DWORD i, state;

        for(i = 0; i < This->primary.nbuffers; ++i)
        {
            DS8Buffer *buf = This->primary.buffers[i];
            if(FAILED(IDirectSoundBuffer_GetStatus(&buf->IDirectSoundBuffer8_iface, &state)) ||
               (state&DSBSTATUS_PLAYING))
            {
                WARN("DSSCL_WRITEPRIMARY set with playing buffers!\n");
                hr = DSERR_INVALIDCALL;
                goto out;
            }
            /* Mark buffer as lost */
            buf->bufferlost = 1;
        }
        if(This->primary.write_emu)
        {
            ERR("Why was there a write_emu?\n");
            /* Delete it */
            IDirectSoundBuffer8_Release(This->primary.write_emu);
            This->primary.write_emu = NULL;
        }
        if(This->primary.flags)
        {
            /* Primary has open references.. create write_emu */
            DSBUFFERDESC desc;
            DS8Buffer *emu;

            memset(&desc, 0, sizeof(desc));
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DSBCAPS_LOCHARDWARE | (This->primary.flags&DSBCAPS_CTRLPAN);
            desc.dwBufferBytes = This->primary.buf_size;
            desc.lpwfxFormat = &This->primary.format.Format;

            hr = DS8Buffer_Create(&emu, &This->primary, NULL);
            if(SUCCEEDED(hr))
            {
                This->primary.write_emu = &emu->IDirectSoundBuffer8_iface;
                hr = IDirectSoundBuffer8_Initialize(This->primary.write_emu, &This->IDirectSound_iface, &desc);
                if(FAILED(hr))
                {
                    IDirectSoundBuffer8_Release(This->primary.write_emu);
                    This->primary.write_emu = NULL;
                }
            }
        }
    }
    else if(This->prio_level == DSSCL_WRITEPRIMARY && level != DSSCL_WRITEPRIMARY && This->primary.write_emu)
    {
        TRACE("Nuking write_emu\n");
        /* Delete it */
        IDirectSoundBuffer8_Release(This->primary.write_emu);
        This->primary.write_emu = NULL;
    }
    if(SUCCEEDED(hr))
        This->prio_level = level;
out:
    LeaveCriticalSection(This->primary.crst);

    return hr;
}

static HRESULT WINAPI DS8_Compact(IDirectSound8 *iface)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->()\n", iface);

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    EnterCriticalSection(&This->share->crst);
    if(This->prio_level < DSSCL_PRIORITY)
    {
        WARN("Coop level not high enough (%lu)\n", This->prio_level);
        hr = DSERR_PRIOLEVELNEEDED;
    }
    LeaveCriticalSection(&This->share->crst);

    return hr;
}

static HRESULT WINAPI DS8_GetSpeakerConfig(IDirectSound8 *iface, DWORD *config)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->(%p)\n", iface, config);

    if(!config)
        return DSERR_INVALIDPARAM;
    *config = 0;

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    EnterCriticalSection(&This->share->crst);
    *config = This->speaker_config;
    LeaveCriticalSection(&This->share->crst);

    return hr;
}

static HRESULT WINAPI DS8_SetSpeakerConfig(IDirectSound8 *iface, DWORD config)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    DWORD geo, speaker;
    HKEY key;
    HRESULT hr;

    TRACE("(%p)->(0x%08lx)\n", iface, config);

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    geo = DSSPEAKER_GEOMETRY(config);
    speaker = DSSPEAKER_CONFIG(config);

    if(geo && (geo < DSSPEAKER_GEOMETRY_MIN || geo > DSSPEAKER_GEOMETRY_MAX))
    {
        WARN("Invalid speaker angle %lu\n", geo);
        return DSERR_INVALIDPARAM;
    }
    if(speaker < DSSPEAKER_HEADPHONE || speaker > DSSPEAKER_7POINT1)
    {
        WARN("Invalid speaker config %lu\n", speaker);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(&This->share->crst);

    hr = DSERR_GENERIC;
    if(!RegCreateKeyExW(HKEY_LOCAL_MACHINE, speakerconfigkey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
    {
        RegSetValueExW(key, speakerconfig, 0, REG_DWORD, (const BYTE*)&config, sizeof(DWORD));
        This->speaker_config = config;
        RegCloseKey(key);
        hr = S_OK;
    }

    LeaveCriticalSection(&This->share->crst);
    return hr;
}

static HRESULT WINAPI DS8_Initialize(IDirectSound8 *iface, const GUID *devguid)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);
    HRESULT hr;
    GUID guid;
    UINT n;

    TRACE("(%p)->(%s)\n", iface, debugstr_guid(devguid));

    if(!openal_loaded)
        return DSERR_NODRIVER;

    if(This->share)
    {
        WARN("Device already initialized\n");
        return DSERR_ALREADYINITIALIZED;
    }

    if(!devguid)
        devguid = &DSDEVID_DefaultPlayback;
    hr = GetDeviceID(devguid, &guid);
    if(FAILED(hr))
        return hr;

    EnterCriticalSection(&openal_crst);

    for(n = 0;n < sharelistsize;n++)
    {
        if(IsEqualGUID(&sharelist[n]->guid, &guid))
        {
            TRACE("Matched already open device %p\n", sharelist[n]->device);

            This->share = sharelist[n];
            DSShare_AddRef(This->share);
            break;
        }
    }

    if(!This->share)
        hr = DSShare_Create(&guid, &This->share);
    if(SUCCEEDED(hr))
    {
        This->device = This->share->device;
        hr = DS8Primary_PreInit(&This->primary, This);
    }

    if(FAILED(hr))
    {
        if(This->share)
            DSShare_Release(This->share);
        This->share = NULL;
    }

    LeaveCriticalSection(&openal_crst);
    return hr;
}

/* I, Maarten Lankhorst, hereby declare this driver certified
 * What this means.. ? An extra bit set
 */
static HRESULT WINAPI DS8_VerifyCertification(IDirectSound8 *iface, DWORD *certified)
{
    DS8Impl *This = impl_from_IDirectSound8(iface);

    TRACE("(%p)->(%p)\n", iface, certified);

    if(!certified)
        return DSERR_INVALIDPARAM;
    *certified = 0;

    if(!This->share)
    {
        WARN("Device not initialized\n");
        return DSERR_UNINITIALIZED;
    }

    *certified = DS_CERTIFIED;

    return DS_OK;
}

static const IDirectSound8Vtbl DS8_Vtbl = {
    DS8_QueryInterface,
    DS8_AddRef,
    DS8_Release,
    DS8_CreateSoundBuffer,
    DS8_GetCaps,
    DS8_DuplicateSoundBuffer,
    DS8_SetCooperativeLevel,
    DS8_Compact,
    DS8_GetSpeakerConfig,
    DS8_SetSpeakerConfig,
    DS8_Initialize,
    DS8_VerifyCertification
};


static HRESULT WINAPI DS_QueryInterface(IDirectSound *iface, REFIID riid, LPVOID *ppv)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_QueryInterface(&This->IDirectSound8_iface, riid, ppv);
}

static ULONG WINAPI DS_AddRef(IDirectSound *iface)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_AddRef(&This->IDirectSound8_iface);
}

static ULONG WINAPI DS_Release(IDirectSound *iface)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_Release(&This->IDirectSound8_iface);
}

static HRESULT WINAPI DS_CreateSoundBuffer(IDirectSound *iface, LPCDSBUFFERDESC desc, LPLPDIRECTSOUNDBUFFER buf, IUnknown *pUnkOuter)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_CreateSoundBuffer(&This->IDirectSound8_iface, desc, buf, pUnkOuter);
}

static HRESULT WINAPI DS_GetCaps(IDirectSound *iface, LPDSCAPS caps)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_GetCaps(&This->IDirectSound8_iface, caps);
}
static HRESULT WINAPI DS_DuplicateSoundBuffer(IDirectSound *iface, IDirectSoundBuffer *in, IDirectSoundBuffer **out)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_DuplicateSoundBuffer(&This->IDirectSound8_iface, in, out);
}

static HRESULT WINAPI DS_SetCooperativeLevel(IDirectSound *iface, HWND hwnd, DWORD level)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_SetCooperativeLevel(&This->IDirectSound8_iface, hwnd, level);
}

static HRESULT WINAPI DS_Compact(IDirectSound *iface)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_Compact(&This->IDirectSound8_iface);
}

static HRESULT WINAPI DS_GetSpeakerConfig(IDirectSound *iface, DWORD *config)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_GetSpeakerConfig(&This->IDirectSound8_iface, config);
}

static HRESULT WINAPI DS_SetSpeakerConfig(IDirectSound *iface, DWORD config)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_SetSpeakerConfig(&This->IDirectSound8_iface, config);
}

static HRESULT WINAPI DS_Initialize(IDirectSound *iface, const GUID *devguid)
{
    DS8Impl *This = impl_from_IDirectSound(iface);
    return DS8_Initialize(&This->IDirectSound8_iface, devguid);
}

static const IDirectSoundVtbl DS_Vtbl = {
    DS_QueryInterface,
    DS_AddRef,
    DS_Release,
    DS_CreateSoundBuffer,
    DS_GetCaps,
    DS_DuplicateSoundBuffer,
    DS_SetCooperativeLevel,
    DS_Compact,
    DS_GetSpeakerConfig,
    DS_SetSpeakerConfig,
    DS_Initialize
};
