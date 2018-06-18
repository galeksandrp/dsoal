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

#define CONST_VTABLE
#include <stdarg.h>
#include <string.h>

#include <windows.h>
#include <dsound.h>
#include <mmsystem.h>

#include "dsound_private.h"

DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED            ((HRESULT)0x80070490)
#endif


static const IDirectSoundBufferVtbl DS8Primary_Vtbl;
static const IDirectSound3DListenerVtbl DS8Primary3D_Vtbl;
static const IKsPropertySetVtbl DS8PrimaryProp_Vtbl;


static inline DS8Primary *impl_from_IDirectSoundBuffer(IDirectSoundBuffer *iface)
{
    return CONTAINING_RECORD(iface, DS8Primary, IDirectSoundBuffer_iface);
}

static inline DS8Primary *impl_from_IDirectSound3DListener(IDirectSound3DListener *iface)
{
    return CONTAINING_RECORD(iface, DS8Primary, IDirectSound3DListener_iface);
}

static inline DS8Primary *impl_from_IKsPropertySet(IKsPropertySet *iface)
{
    return CONTAINING_RECORD(iface, DS8Primary, IKsPropertySet_iface);
}


static void AL_APIENTRY wrap_DeferUpdates(void)
{ alcSuspendContext(alcGetCurrentContext()); }
static void AL_APIENTRY wrap_ProcessUpdates(void)
{ alcProcessContext(alcGetCurrentContext()); }


static void trigger_elapsed_notifies(DS8Buffer *buf, DWORD lastpos, DWORD curpos)
{
    DSBPOSITIONNOTIFY *not = buf->notify;
    DSBPOSITIONNOTIFY *not_end = not + buf->nnotify;
    for(;not != not_end;++not)
    {
        HANDLE event = not->hEventNotify;
        DWORD ofs = not->dwOffset;

        if(ofs == (DWORD)DSBPN_OFFSETSTOP)
            continue;

        if(curpos < lastpos) /* Wraparound case */
        {
            if(ofs < curpos || ofs >= lastpos)
            {
                TRACE("Triggering notification %d from buffer %p\n", not - buf->notify, buf);
                SetEvent(event);
            }
        }
        else if(ofs >= lastpos && ofs < curpos) /* Normal case */
        {
            TRACE("Triggering notification %d from buffer %p\n", not - buf->notify, buf);
            SetEvent(event);
        }
    }
}

static void trigger_stop_notifies(DS8Buffer *buf)
{
    DSBPOSITIONNOTIFY *not = buf->notify;
    DSBPOSITIONNOTIFY *not_end = not + buf->nnotify;
    for(;not != not_end;++not)
    {
        if(not->dwOffset != (DWORD)DSBPN_OFFSETSTOP)
            continue;
        TRACE("Triggering notification %d from buffer %p\n", not - buf->notify, buf);
        SetEvent(not->hEventNotify);
    }
}

void DS8Primary_triggernots(DS8Primary *prim)
{
    DS8Buffer **curnot, **endnot;

    curnot = prim->notifies;
    endnot = curnot + prim->nnotifies;
    while(curnot != endnot)
    {
        DS8Buffer *buf = *curnot;
        DS8Data *data = buf->buffer;
        DWORD curpos = buf->lastpos;
        ALint state = 0;
        ALint ofs;

        alGetSourcei(buf->source, AL_BYTE_OFFSET, &ofs);
        alGetSourcei(buf->source, AL_SOURCE_STATE, &state);
        if(buf->segsize == 0)
            curpos = (state == AL_STOPPED) ? data->buf_size : ofs;
        else
        {
            if(state != AL_STOPPED)
                curpos = ofs + buf->queue_base;
            else
            {
                ALint queued;
                alGetSourcei(buf->source, AL_BUFFERS_QUEUED, &queued);
                curpos = buf->segsize*queued + buf->queue_base;
            }

            if(curpos >= (DWORD)data->buf_size)
            {
                if(buf->islooping)
                    curpos %= (DWORD)data->buf_size;
                else if(buf->isplaying)
                {
                    curpos = data->buf_size;
                    alSourceStop(buf->source);
                    alSourcei(buf->source, AL_BUFFER, 0);
                    buf->curidx = 0;
                    buf->isplaying = FALSE;
                }
            }

            if(state != AL_PLAYING)
                state = buf->isplaying ? AL_PLAYING : AL_PAUSED;
        }
        checkALError();

        if(buf->lastpos != curpos)
        {
            trigger_elapsed_notifies(buf, buf->lastpos, curpos);
            buf->lastpos = curpos;
        }
        if(state != AL_PLAYING)
        {
            /* Remove this buffer from list and put another at the current
             * position; don't increment i
             */
            trigger_stop_notifies(buf);
            *curnot = *(--endnot);
            prim->nnotifies--;
            continue;
        }
        curnot++;
    }
    checkALError();
}

static void do_buffer_stream(DS8Buffer *buf, BYTE *scratch_mem)
{
    DS8Data *data = buf->buffer;
    ALint ofs, done = 0, queued = QBUFFERS, state = AL_PLAYING;
    ALuint which;

    alGetSourcei(buf->source, AL_BUFFERS_QUEUED, &queued);
    alGetSourcei(buf->source, AL_SOURCE_STATE, &state);
    alGetSourcei(buf->source, AL_BUFFERS_PROCESSED, &done);

    if(done > 0)
    {
        ALuint bids[QBUFFERS];
        queued -= done;

        alSourceUnqueueBuffers(buf->source, done, bids);
        buf->queue_base = (buf->queue_base + buf->segsize*done) % data->buf_size;
    }
    while(queued < QBUFFERS)
    {
        which = buf->stream_bids[buf->curidx];
        ofs = buf->data_offset;

        if(buf->segsize < data->buf_size - ofs)
        {
            alBufferData(which, data->buf_format, data->data + ofs, buf->segsize,
                            data->format.Format.nSamplesPerSec);
            buf->data_offset = ofs + buf->segsize;
        }
        else if(buf->islooping)
        {
            ALsizei rem = data->buf_size - ofs;
            if(rem > 2048) rem = 2048;

            memcpy(scratch_mem, data->data + ofs, rem);
            while(rem < buf->segsize)
            {
                ALsizei todo = buf->segsize - rem;
                if(todo > data->buf_size)
                    todo = data->buf_size;
                memcpy(scratch_mem + rem, data->data, todo);
                rem += todo;
            }
            alBufferData(which, data->buf_format, scratch_mem, buf->segsize,
                            data->format.Format.nSamplesPerSec);
            buf->data_offset = (ofs+buf->segsize) % data->buf_size;
        }
        else
        {
            ALsizei rem = data->buf_size - ofs;
            if(rem > 2048) rem = 2048;
            if(rem == 0) break;

            memcpy(scratch_mem, data->data + ofs, rem);
            memset(scratch_mem+rem, (data->format.Format.wBitsPerSample==8) ? 128 : 0,
                    buf->segsize - rem);
            alBufferData(which, data->buf_format, scratch_mem, buf->segsize,
                            data->format.Format.nSamplesPerSec);
            buf->data_offset = data->buf_size;
        }

        alSourceQueueBuffers(buf->source, 1, &which);
        buf->curidx = (buf->curidx+1)%QBUFFERS;
        queued++;
    }

    if(!queued)
    {
        buf->data_offset = 0;
        buf->queue_base = data->buf_size;
        buf->curidx = 0;
        buf->isplaying = FALSE;
    }
    else if(state != AL_PLAYING)
        alSourcePlay(buf->source);
}

void DS8Primary_streamfeeder(DS8Primary *prim, BYTE *scratch_mem)
{
    /* OpenAL doesn't support our lovely buffer extensions so just make sure
     * enough buffers are queued for streaming
     */
    if(prim->write_emu)
    {
        DS8Buffer *buf = &prim->writable_buf;
        if(buf->segsize != 0 && buf->isplaying)
            do_buffer_stream(buf, scratch_mem);
    }
    else
    {
        struct DSBufferGroup *bufgroup = prim->BufferGroups;
        struct DSBufferGroup *endgroup = bufgroup + prim->NumBufferGroups;
        for(;bufgroup != endgroup;++bufgroup)
        {
            DWORD64 usemask = ~bufgroup->FreeBuffers;
            while(usemask)
            {
                int idx = CTZ64(usemask);
                DS8Buffer *buf = bufgroup->Buffers + idx;
                usemask &= ~(U64(1) << idx);

                if(buf->segsize != 0 && buf->isplaying)
                    do_buffer_stream(buf, scratch_mem);
            }
        }
    }
    checkALError();
}


HRESULT DS8Primary_PreInit(DS8Primary *This, DS8Impl *parent)
{
    DS3DLISTENER *listener;
    WAVEFORMATEX *wfx;
    DWORD num_srcs;
    DWORD count;
    HRESULT hr;
    DWORD i;

    This->IDirectSoundBuffer_iface.lpVtbl = &DS8Primary_Vtbl;
    This->IDirectSound3DListener_iface.lpVtbl = &DS8Primary3D_Vtbl;
    This->IKsPropertySet_iface.lpVtbl = &DS8PrimaryProp_Vtbl;

    This->parent = parent;
    This->crst = &parent->share->crst;
    This->ctx = parent->share->ctx;
    This->refresh = parent->share->refresh;
    This->SupportedExt = parent->share->SupportedExt;
    This->ExtAL = &parent->share->ExtAL;
    This->sources = parent->share->sources;
    This->auxslot = parent->share->auxslot;

    wfx = &This->format.Format;
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = 2;
    wfx->wBitsPerSample = 8;
    wfx->nSamplesPerSec = 22050;
    wfx->nBlockAlign = wfx->wBitsPerSample * wfx->nChannels / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize = 0;

    This->stopped = TRUE;

    /* Apparently primary buffer size is always 32k,
     * tested on windows with 192k 24 bits sound @ 6 channels
     * where it will run out in 60 ms and it isn't pointer aligned
     */
    This->buf_size = 32768;

    setALContext(This->ctx);
    if(This->SupportedExt[SOFT_DEFERRED_UPDATES])
    {
        This->DeferUpdates = This->ExtAL->DeferUpdatesSOFT;
        This->ProcessUpdates = This->ExtAL->ProcessUpdatesSOFT;
    }
    else
    {
        This->DeferUpdates = wrap_DeferUpdates;
        This->ProcessUpdates = wrap_ProcessUpdates;
    }

    This->eax_prop = EnvironmentDefaults[EAX_ENVIRONMENT_GENERIC];
    if(This->SupportedExt[EXT_EFX] && This->auxslot != 0)
    {
        ALint revid = alGetEnumValue("AL_EFFECT_REVERB");
        if(revid != 0 && revid != -1)
        {
            This->ExtAL->GenEffects(1, &This->effect);
            This->ExtAL->Effecti(This->effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
            checkALError();
        }
    }
    popALContext();

    listener = &This->params;
    listener->dwSize = sizeof(This->params);
    listener->vPosition.x = 0.0f;
    listener->vPosition.y = 0.0f;
    listener->vPosition.z = 0.0f;
    listener->vVelocity.x = 0.0f;
    listener->vVelocity.y = 0.0f;
    listener->vVelocity.z = 0.0f;
    listener->vOrientFront.x = 0.0f;
    listener->vOrientFront.y = 0.0f;
    listener->vOrientFront.z = 1.0f;
    listener->vOrientTop.x = 0.0f;
    listener->vOrientTop.y = 1.0f;
    listener->vOrientTop.z = 0.0f;
    listener->flDistanceFactor = DS3D_DEFAULTDISTANCEFACTOR;
    listener->flRolloffFactor = DS3D_DEFAULTROLLOFFFACTOR;
    listener->flDopplerFactor = DS3D_DEFAULTDOPPLERFACTOR;

    num_srcs = parent->share->max_sources;

    hr = DSERR_OUTOFMEMORY;
    This->notifies = HeapAlloc(GetProcessHeap(), 0, num_srcs*sizeof(*This->notifies));
    if(!This->notifies) goto fail;
    This->sizenotifies = num_srcs;

    count = (num_srcs+63) / 64;
    This->BufferGroups = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   count*sizeof(*This->BufferGroups));
    if(!This->BufferGroups) goto fail;
    This->NumBufferGroups = count;

    /* Only flag usable buffers as free. */
    count = 0;
    for(i = 0;i < This->NumBufferGroups;++i)
    {
        DWORD count_rem = num_srcs - count;
        if(count_rem >= 64)
        {
            This->BufferGroups[i].FreeBuffers = ~(DWORD64)0;
            count += 64;
        }
        else
        {
            This->BufferGroups[i].FreeBuffers = (U64(1) << count_rem) - 1;
            count += count_rem;
        }
    }

    return S_OK;

fail:
    DS8Primary_Clear(This);
    return hr;
}

void DS8Primary_Clear(DS8Primary *This)
{
    struct DSBufferGroup *bufgroup;
    DWORD i;

    TRACE("Clearing primary %p\n", This);

    if(!This->parent)
        return;

    setALContext(This->ctx);
    if(This->effect)
        This->ExtAL->DeleteEffects(1, &This->effect);
    popALContext();

    bufgroup = This->BufferGroups;
    for(i = 0;i < This->NumBufferGroups;++i)
    {
        DWORD64 usemask = ~bufgroup[i].FreeBuffers;
        while(usemask)
        {
            int idx = CTZ64(usemask);
            DS8Buffer *buf = bufgroup[i].Buffers + idx;
            usemask &= ~(U64(1) << idx);

            DS8Buffer_Destroy(buf);
        }
    }

    HeapFree(GetProcessHeap(), 0, This->BufferGroups);
    HeapFree(GetProcessHeap(), 0, This->notifies);
    memset(This, 0, sizeof(*This));
}

static HRESULT WINAPI DS8Primary_QueryInterface(IDirectSoundBuffer *iface, REFIID riid, LPVOID *ppv)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);

    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    *ppv = NULL;
    if(IsEqualIID(riid, &IID_IUnknown) ||
       IsEqualIID(riid, &IID_IDirectSoundBuffer))
        *ppv = &This->IDirectSoundBuffer_iface;
    else if(IsEqualIID(riid, &IID_IDirectSound3DListener))
    {
        if((This->flags&DSBCAPS_CTRL3D))
            *ppv = &This->IDirectSound3DListener_iface;
    }
    else if(IsEqualIID(riid, &IID_IKsPropertySet))
        *ppv = &This->IKsPropertySet_iface;
    else
        FIXME("Unhandled GUID: %s\n", debugstr_guid(riid));

    if(*ppv)
    {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI DS8Primary_AddRef(IDirectSoundBuffer *iface)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    LONG ret;

    ret = InterlockedIncrement(&This->ref);
    if(ret == 1) This->flags = 0;

    return ret;
}

static ULONG WINAPI DS8Primary_Release(IDirectSoundBuffer *iface)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    LONG ref, oldval;

    oldval = *(volatile LONG*)&This->ref;
    do {
        ref = oldval;
        if(!ref) return 0;
        oldval = InterlockedCompareExchange(&This->ref, ref-1, ref);
    } while(oldval != ref);

    return ref-1;
}

static HRESULT WINAPI DS8Primary_GetCaps(IDirectSoundBuffer *iface, DSBCAPS *caps)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);

    TRACE("(%p)->(%p)\n", iface, caps);

    if(!caps || caps->dwSize < sizeof(*caps))
    {
        WARN("Invalid DSBCAPS (%p, %lu)\n", caps, caps ? caps->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    caps->dwFlags = This->flags;
    caps->dwBufferBytes = This->buf_size;
    caps->dwUnlockTransferRate = 0;
    caps->dwPlayCpuOverhead = 0;

    return DS_OK;
}

static HRESULT WINAPI DS8Primary_GetCurrentPosition(IDirectSoundBuffer *iface, DWORD *playpos, DWORD *curpos)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = DSERR_PRIOLEVELNEEDED;

    EnterCriticalSection(This->crst);
    if(This->write_emu)
        hr = IDirectSoundBuffer8_GetCurrentPosition(This->write_emu, playpos, curpos);
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_GetFormat(IDirectSoundBuffer *iface, WAVEFORMATEX *wfx, DWORD allocated, DWORD *written)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = S_OK;
    UINT size;

    if(!wfx && !written)
    {
        WARN("Cannot report format or format size\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    size = sizeof(This->format.Format) + This->format.Format.cbSize;
    if(written)
        *written = size;
    if(wfx)
    {
        if(allocated < size)
            hr = DSERR_INVALIDPARAM;
        else
            memcpy(wfx, &This->format.Format, size);
    }
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_GetVolume(IDirectSoundBuffer *iface, LONG *volume)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    ALfloat gain;

    TRACE("(%p)->(%p)\n", iface, volume);

    if(!volume)
        return DSERR_INVALIDPARAM;
    *volume = 0;

    if(!(This->flags&DSBCAPS_CTRLVOLUME))
        return DSERR_CONTROLUNAVAIL;

    setALContext(This->ctx);
    alGetListenerf(AL_GAIN, &gain);
    checkALError();
    popALContext();

    *volume = clampI(gain_to_mB(gain), DSBVOLUME_MIN, DSBVOLUME_MAX);
    return DS_OK;
}

static HRESULT WINAPI DS8Primary_GetPan(IDirectSoundBuffer *iface, LONG *pan)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = DS_OK;

    WARN("(%p)->(%p): semi-stub\n", iface, pan);

    if(!pan)
        return DSERR_INVALIDPARAM;

    EnterCriticalSection(This->crst);
    if(This->write_emu)
        hr = IDirectSoundBuffer8_GetPan(This->write_emu, pan);
    else if(!(This->flags & DSBCAPS_CTRLPAN))
        hr = DSERR_CONTROLUNAVAIL;
    else
        *pan = 0;
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_GetFrequency(IDirectSoundBuffer *iface, DWORD *freq)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = DS_OK;

    WARN("(%p)->(%p): semi-stub\n", iface, freq);

    if(!freq)
        return DSERR_INVALIDPARAM;

    if(!(This->flags&DSBCAPS_CTRLFREQUENCY))
        return DSERR_CONTROLUNAVAIL;

    EnterCriticalSection(This->crst);
    *freq = This->format.Format.nSamplesPerSec;
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_GetStatus(IDirectSoundBuffer *iface, DWORD *status)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);

    TRACE("(%p)->(%p)\n", iface, status);

    if(!status)
        return DSERR_INVALIDPARAM;

    EnterCriticalSection(This->crst);
    *status = DSBSTATUS_PLAYING|DSBSTATUS_LOOPING;
    if((This->flags&DSBCAPS_LOCDEFER))
        *status |= DSBSTATUS_LOCHARDWARE;

    if(This->stopped)
    {
        struct DSBufferGroup *bufgroup = This->BufferGroups;
        DWORD i, state = 0;
        HRESULT hr;

        for(i = 0;i < This->NumBufferGroups;++i)
        {
            DWORD64 usemask = ~bufgroup[i].FreeBuffers;
            while(usemask)
            {
                int idx = CTZ64(usemask);
                DS8Buffer *buf = bufgroup[i].Buffers + idx;
                usemask &= ~(U64(1) << idx);

                hr = DS8Buffer_GetStatus(&buf->IDirectSoundBuffer8_iface, &state);
                if(SUCCEEDED(hr) && (state&DSBSTATUS_PLAYING)) break;
            }
        }
        if(!(state&DSBSTATUS_PLAYING))
        {
            /* Primary stopped and no buffers playing.. */
            *status = 0;
        }
    }
    LeaveCriticalSection(This->crst);

    return DS_OK;
}

HRESULT WINAPI DS8Primary_Initialize(IDirectSoundBuffer *iface, IDirectSound *ds, const DSBUFFERDESC *desc)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", iface, ds, desc);

    if(!desc || desc->lpwfxFormat || desc->dwBufferBytes)
    {
        WARN("Bad DSBDESC for primary buffer\n");
        return DSERR_INVALIDPARAM;
    }
    if((desc->dwFlags&DSBCAPS_CTRLFX) ||
       (desc->dwFlags&DSBCAPS_CTRLPOSITIONNOTIFY) ||
       (desc->dwFlags&DSBCAPS_LOCSOFTWARE))
    {
        WARN("Bad dwFlags %08lx\n", desc->dwFlags);
        return DSERR_INVALIDPARAM;
    }

    /* Should be 0 if not initialized */
    if(This->flags)
        return DSERR_ALREADYINITIALIZED;

    hr = DS_OK;
    if(This->parent->prio_level == DSSCL_WRITEPRIMARY)
    {
        DSBUFFERDESC emudesc;
        DS8Buffer *emu;

        if(This->write_emu)
        {
            ERR("There shouldn't be a write_emu!\n");
            IDirectSoundBuffer8_Release(This->write_emu);
            This->write_emu = NULL;
        }

        memset(&emudesc, 0, sizeof(emudesc));
        emudesc.dwSize = sizeof(emudesc);
        emudesc.dwFlags = DSBCAPS_LOCHARDWARE | (desc->dwFlags&DSBCAPS_CTRLPAN);
        /* Dont play last incomplete sample */
        emudesc.dwBufferBytes = This->buf_size - (This->buf_size%This->format.Format.nBlockAlign);
        emudesc.lpwfxFormat = &This->format.Format;

        hr = DS8Buffer_Create(&emu, This, NULL, TRUE);
        if(SUCCEEDED(hr))
        {
            This->write_emu = &emu->IDirectSoundBuffer8_iface;
            hr = DS8Buffer_Initialize(This->write_emu, ds, &emudesc);
            if(FAILED(hr))
            {
                IDirectSoundBuffer8_Release(This->write_emu);
                This->write_emu = NULL;
            }
        }
    }

    if(SUCCEEDED(hr))
        This->flags = desc->dwFlags | DSBCAPS_LOCHARDWARE;
    return hr;
}

static HRESULT WINAPI DS8Primary_Lock(IDirectSoundBuffer *iface, DWORD ofs, DWORD bytes, void **ptr1, DWORD *len1, void **ptr2, DWORD *len2, DWORD flags)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = DSERR_PRIOLEVELNEEDED;

    TRACE("(%p)->(%lu, %lu, %p, %p, %p, %p, %lu)\n", iface, ofs, bytes, ptr1, len1, ptr2, len2, flags);

    EnterCriticalSection(This->crst);
    if(This->write_emu)
        hr = IDirectSoundBuffer8_Lock(This->write_emu, ofs, bytes, ptr1, len1, ptr2, len2, flags);
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_Play(IDirectSoundBuffer *iface, DWORD res1, DWORD res2, DWORD flags)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr;

    TRACE("(%p)->(%lu, %lu, %lu)\n", iface, res1, res2, flags);

    if(!(flags & DSBPLAY_LOOPING))
    {
        WARN("Flags (%08lx) not set to DSBPLAY_LOOPING\n", flags);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    hr = S_OK;
    if(This->write_emu)
        hr = IDirectSoundBuffer8_Play(This->write_emu, res1, res2, flags);
    if(SUCCEEDED(hr))
        This->stopped = FALSE;
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_SetCurrentPosition(IDirectSoundBuffer *iface, DWORD pos)
{
    WARN("(%p)->(%lu)\n", iface, pos);
    return DSERR_INVALIDCALL;
}

/* Just assume the format is crap, and clean up the damage */
static HRESULT copy_waveformat(WAVEFORMATEX *wfx, const WAVEFORMATEX *from)
{
    if(from->nChannels <= 0)
    {
        WARN("Invalid Channels %d\n", from->nChannels);
        return DSERR_INVALIDPARAM;
    }
    if(from->nSamplesPerSec < DSBFREQUENCY_MIN || from->nSamplesPerSec > DSBFREQUENCY_MAX)
    {
        WARN("Invalid SamplesPerSec %lu\n", from->nSamplesPerSec);
        return DSERR_INVALIDPARAM;
    }
    if(from->nBlockAlign <= 0)
    {
        WARN("Invalid BlockAlign %d\n", from->nBlockAlign);
        return DSERR_INVALIDPARAM;
    }
    if(from->wBitsPerSample == 0 || (from->wBitsPerSample%8) != 0)
    {
        WARN("Invalid BitsPerSample %d\n", from->wBitsPerSample);
        return DSERR_INVALIDPARAM;
    }
    if(from->nBlockAlign != from->nChannels*from->wBitsPerSample/8)
    {
        WARN("Invalid BlockAlign %d (expected %u = %u*%u/8)\n",
             from->nBlockAlign, from->nChannels*from->wBitsPerSample/8,
             from->nChannels, from->wBitsPerSample);
        return DSERR_INVALIDPARAM;
    }
    if(from->nAvgBytesPerSec != from->nBlockAlign*from->nSamplesPerSec)
    {
        WARN("Invalid AvgBytesPerSec %lu (expected %lu = %lu*%u)\n",
             from->nAvgBytesPerSec, from->nSamplesPerSec*from->nBlockAlign,
             from->nSamplesPerSec, from->nBlockAlign);
        return DSERR_INVALIDPARAM;
    }

    if(from->wFormatTag == WAVE_FORMAT_PCM)
    {
        if(from->wBitsPerSample > 32)
            return DSERR_INVALIDPARAM;
        wfx->cbSize = 0;
    }
    else if(from->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        if(from->wBitsPerSample != 32)
            return DSERR_INVALIDPARAM;
        wfx->cbSize = 0;
    }
    else if(from->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        WAVEFORMATEXTENSIBLE *wfe = (WAVEFORMATEXTENSIBLE*)wfx;
        const WAVEFORMATEXTENSIBLE *fromx = (const WAVEFORMATEXTENSIBLE*)from;
        const WORD size = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

        /* Fail silently.. */
        if(from->cbSize < size) return DS_OK;
        if(fromx->Samples.wValidBitsPerSample > fromx->Format.wBitsPerSample)
            return DSERR_INVALIDPARAM;

        if(IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
           IsEqualGUID(&wfe->SubFormat, &GUID_NULL))
        {
            if(from->wBitsPerSample > 32)
                return DSERR_INVALIDPARAM;
        }
        else if(IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        {
            if(from->wBitsPerSample != 32)
                return DSERR_INVALIDPARAM;
        }
        else
        {
            ERR("Unhandled extensible format: %s\n", debugstr_guid(&wfe->SubFormat));
            return DSERR_INVALIDPARAM;
        }

        wfe->Format.cbSize = size;
        wfe->Samples.wValidBitsPerSample = fromx->Samples.wValidBitsPerSample;
        if(!wfe->Samples.wValidBitsPerSample)
            wfe->Samples.wValidBitsPerSample = wfe->Format.wBitsPerSample;
        wfe->dwChannelMask = fromx->dwChannelMask;
        wfe->SubFormat = fromx->SubFormat;
    }
    else
    {
        ERR("Unhandled format tag %04x\n", from->wFormatTag);
        return DSERR_INVALIDPARAM;
    }

    wfx->wFormatTag = from->wFormatTag;
    wfx->nChannels = from->nChannels;
    wfx->nSamplesPerSec = from->nSamplesPerSec;
    wfx->nAvgBytesPerSec = from->nSamplesPerSec * from->nBlockAlign;
    wfx->nBlockAlign = from->wBitsPerSample * from->nChannels / 8;
    wfx->wBitsPerSample = from->wBitsPerSample;
    return DS_OK;
}

static HRESULT WINAPI DS8Primary_SetFormat(IDirectSoundBuffer *iface, const WAVEFORMATEX *wfx)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->(%p)\n", iface, wfx);

    if(!wfx)
    {
        WARN("Missing format\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);

    if(This->parent->prio_level < DSSCL_PRIORITY)
    {
        hr = DSERR_PRIOLEVELNEEDED;
        goto out;
    }

    TRACE("Requested primary format:\n"
          "    FormatTag      = %04x\n"
          "    Channels       = %u\n"
          "    SamplesPerSec  = %lu\n"
          "    AvgBytesPerSec = %lu\n"
          "    BlockAlign     = %u\n"
          "    BitsPerSample  = %u\n",
          wfx->wFormatTag, wfx->nChannels,
          wfx->nSamplesPerSec, wfx->nAvgBytesPerSec,
          wfx->nBlockAlign, wfx->wBitsPerSample);

    hr = copy_waveformat(&This->format.Format, wfx);
    if(SUCCEEDED(hr) && This->write_emu)
    {
        DS8Buffer *buf;
        DSBUFFERDESC desc;

        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_LOCHARDWARE|DSBCAPS_CTRLPAN;
        desc.dwBufferBytes = This->buf_size - (This->buf_size % This->format.Format.nBlockAlign);
        desc.lpwfxFormat = &This->format.Format;

        hr = DS8Buffer_Create(&buf, This, NULL, TRUE);
        if(FAILED(hr)) goto out;

        hr = DS8Buffer_Initialize(&buf->IDirectSoundBuffer8_iface, &This->parent->IDirectSound_iface, &desc);
        if(FAILED(hr))
            DS8Buffer_Destroy(buf);
        else
        {
            IDirectSoundBuffer8_Release(This->write_emu);
            This->write_emu = &buf->IDirectSoundBuffer8_iface;
        }
    }

out:
    LeaveCriticalSection(This->crst);
    return hr;
}

static HRESULT WINAPI DS8Primary_SetVolume(IDirectSoundBuffer *iface, LONG vol)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);

    TRACE("(%p)->(%ld)\n", iface, vol);

    if(vol > DSBVOLUME_MAX || vol < DSBVOLUME_MIN)
    {
        WARN("Invalid volume (%ld)\n", vol);
        return DSERR_INVALIDPARAM;
    }

    if(!(This->flags&DSBCAPS_CTRLVOLUME))
        return DSERR_CONTROLUNAVAIL;

    setALContext(This->ctx);
    alListenerf(AL_GAIN, mB_to_gain(vol));
    popALContext();

    return DS_OK;
}

static HRESULT WINAPI DS8Primary_SetPan(IDirectSoundBuffer *iface, LONG pan)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr;

    TRACE("(%p)->(%ld)\n", iface, pan);

    if(pan > DSBPAN_RIGHT || pan < DSBPAN_LEFT)
    {
        WARN("invalid parameter: pan = %ld\n", pan);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    if(!(This->flags&DSBCAPS_CTRLPAN))
    {
        WARN("control unavailable\n");
        hr = DSERR_CONTROLUNAVAIL;
    }
    else if(This->write_emu)
        hr = IDirectSoundBuffer8_SetPan(This->write_emu, pan);
    else
    {
        FIXME("Not supported\n");
        hr = E_NOTIMPL;
    }
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_SetFrequency(IDirectSoundBuffer *iface, DWORD freq)
{
    WARN("(%p)->(%lu)\n", iface, freq);
    return DSERR_CONTROLUNAVAIL;
}

static HRESULT WINAPI DS8Primary_Stop(IDirectSoundBuffer *iface)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->()\n", iface);

    EnterCriticalSection(This->crst);
    if(This->write_emu)
        hr = IDirectSoundBuffer8_Stop(This->write_emu);
    if(SUCCEEDED(hr))
        This->stopped = TRUE;
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_Unlock(IDirectSoundBuffer *iface, void *ptr1, DWORD len1, void *ptr2, DWORD len2)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = DSERR_INVALIDCALL;

    TRACE("(%p)->(%p, %lu, %p, %lu)\n", iface, ptr1, len1, ptr2, len2);

    EnterCriticalSection(This->crst);
    if(This->write_emu)
        hr = IDirectSoundBuffer8_Unlock(This->write_emu, ptr1, len1, ptr2, len2);
    LeaveCriticalSection(This->crst);

    return hr;
}

static HRESULT WINAPI DS8Primary_Restore(IDirectSoundBuffer *iface)
{
    DS8Primary *This = impl_from_IDirectSoundBuffer(iface);
    HRESULT hr = S_OK;

    TRACE("(%p)->()\n", iface);

    EnterCriticalSection(This->crst);
    if(This->write_emu)
        hr = IDirectSoundBuffer8_Restore(This->write_emu);
    LeaveCriticalSection(This->crst);

    return hr;
}

static const IDirectSoundBufferVtbl DS8Primary_Vtbl =
{
    DS8Primary_QueryInterface,
    DS8Primary_AddRef,
    DS8Primary_Release,
    DS8Primary_GetCaps,
    DS8Primary_GetCurrentPosition,
    DS8Primary_GetFormat,
    DS8Primary_GetVolume,
    DS8Primary_GetPan,
    DS8Primary_GetFrequency,
    DS8Primary_GetStatus,
    DS8Primary_Initialize,
    DS8Primary_Lock,
    DS8Primary_Play,
    DS8Primary_SetCurrentPosition,
    DS8Primary_SetFormat,
    DS8Primary_SetVolume,
    DS8Primary_SetPan,
    DS8Primary_SetFrequency,
    DS8Primary_Stop,
    DS8Primary_Unlock,
    DS8Primary_Restore
};


static void DS8Primary_SetParams(DS8Primary *This, const DS3DLISTENER *params, LONG flags)
{
    union PrimaryParamFlags dirty = { flags };
    DWORD i;

    if(dirty.bit.pos)
        alListener3f(AL_POSITION, params->vPosition.x, params->vPosition.y,
                                 -params->vPosition.z);
    if(dirty.bit.vel)
        alListener3f(AL_VELOCITY, params->vVelocity.x, params->vVelocity.y,
                                 -params->vVelocity.z);
    if(dirty.bit.orientation)
    {
        ALfloat orient[6] = {
            params->vOrientFront.x, params->vOrientFront.y, -params->vOrientFront.z,
            params->vOrientTop.x, params->vOrientTop.y, -params->vOrientTop.z
        };
        alListenerfv(AL_ORIENTATION, orient);
    }
    if(dirty.bit.distancefactor)
    {
        alSpeedOfSound(343.3f/params->flDistanceFactor);
        if(This->SupportedExt[EXT_EFX])
            alListenerf(AL_METERS_PER_UNIT, params->flDistanceFactor);
    }
    if(dirty.bit.rollofffactor)
    {
        struct DSBufferGroup *bufgroup = This->BufferGroups;
        ALfloat rolloff = params->flRolloffFactor;
        This->rollofffactor = rolloff;

        for(i = 0;i < This->NumBufferGroups;++i)
        {
            DWORD64 usemask = ~bufgroup[i].FreeBuffers;
            while(usemask)
            {
                int idx = CTZ64(usemask);
                DS8Buffer *buf = bufgroup[i].Buffers + idx;
                usemask &= ~(U64(1) << idx);

                if(buf->ds3dmode != DS3DMODE_DISABLE)
                    alSourcef(buf->source, AL_ROLLOFF_FACTOR, rolloff);
            }
        }
    }
    if(dirty.bit.dopplerfactor)
        alDopplerFactor(params->flDopplerFactor);
    if(dirty.bit.effect)
        This->ExtAL->AuxiliaryEffectSloti(This->auxslot, AL_EFFECTSLOT_EFFECT, This->effect);
}

static HRESULT WINAPI DS8Primary3D_QueryInterface(IDirectSound3DListener *iface, REFIID riid, void **ppv)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    return DS8Primary_QueryInterface(&This->IDirectSoundBuffer_iface, riid, ppv);
}

static ULONG WINAPI DS8Primary3D_AddRef(IDirectSound3DListener *iface)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    LONG ret;

    ret = InterlockedIncrement(&This->ds3d_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static ULONG WINAPI DS8Primary3D_Release(IDirectSound3DListener *iface)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    LONG ret;

    ret = InterlockedDecrement(&This->ds3d_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}


static HRESULT WINAPI DS8Primary3D_GetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE *distancefactor)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%p)\n", iface, distancefactor);

    if(!distancefactor)
    {
        WARN("Invalid parameter %p\n", distancefactor);
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    *distancefactor = 343.3f/alGetFloat(AL_SPEED_OF_SOUND);
    checkALError();
    popALContext();

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_GetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE *dopplerfactor)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%p)\n", iface, dopplerfactor);

    if(!dopplerfactor)
    {
        WARN("Invalid parameter %p\n", dopplerfactor);
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    *dopplerfactor = alGetFloat(AL_DOPPLER_FACTOR);
    checkALError();
    popALContext();

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_GetOrientation(IDirectSound3DListener *iface, D3DVECTOR *front, D3DVECTOR *top)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    ALfloat orient[6];

    TRACE("(%p)->(%p, %p)\n", iface, front, top);

    if(!front || !top)
    {
        WARN("Invalid parameter %p %p\n", front, top);
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetListenerfv(AL_ORIENTATION, orient);
    checkALError();
    popALContext();

    front->x =  orient[0];
    front->y =  orient[1];
    front->z = -orient[2];
    top->x =  orient[3];
    top->y =  orient[4];
    top->z = -orient[5];
    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_GetPosition(IDirectSound3DListener *iface, D3DVECTOR *pos)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    ALfloat alpos[3];

    TRACE("(%p)->(%p)\n", iface, pos);

    if(!pos)
    {
        WARN("Invalid parameter %p\n", pos);
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetListenerfv(AL_POSITION, alpos);
    checkALError();
    popALContext();

    pos->x =  alpos[0];
    pos->y =  alpos[1];
    pos->z = -alpos[2];
    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_GetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE *rollofffactor)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%p)\n", iface, rollofffactor);

    if(!rollofffactor)
    {
        WARN("Invalid parameter %p\n", rollofffactor);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    *rollofffactor = This->rollofffactor;
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_GetVelocity(IDirectSound3DListener *iface, D3DVECTOR *velocity)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    ALfloat vel[3];

    TRACE("(%p)->(%p)\n", iface, velocity);

    if(!velocity)
    {
        WARN("Invalid parameter %p\n", velocity);
        return DSERR_INVALIDPARAM;
    }

    setALContext(This->ctx);
    alGetListenerfv(AL_VELOCITY, vel);
    checkALError();
    popALContext();

    velocity->x =  vel[0];
    velocity->y =  vel[1];
    velocity->z = -vel[2];
    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_GetAllParameters(IDirectSound3DListener *iface, DS3DLISTENER *listener)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%p)\n", iface, listener);

    if(!listener || listener->dwSize < sizeof(*listener))
    {
        WARN("Invalid DS3DLISTENER %p %lu\n", listener, listener ? listener->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);
    DS8Primary3D_GetPosition(iface, &listener->vPosition);
    DS8Primary3D_GetVelocity(iface, &listener->vVelocity);
    DS8Primary3D_GetOrientation(iface, &listener->vOrientFront, &listener->vOrientTop);
    DS8Primary3D_GetDistanceFactor(iface, &listener->flDistanceFactor);
    DS8Primary3D_GetRolloffFactor(iface, &listener->flRolloffFactor);
    DS8Primary3D_GetDopplerFactor(iface, &listener->flDopplerFactor);
    popALContext();
    LeaveCriticalSection(This->crst);

    return DS_OK;
}


static HRESULT WINAPI DS8Primary3D_SetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE factor, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%f, %lu)\n", iface, factor, apply);

    if(factor < DS3D_MINDISTANCEFACTOR ||
       factor > DS3D_MAXDISTANCEFACTOR)
    {
        WARN("Invalid parameter %f\n", factor);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.flDistanceFactor = factor;
        This->dirty.bit.distancefactor = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alSpeedOfSound(343.3f/factor);
        if(This->SupportedExt[EXT_EFX])
            alListenerf(AL_METERS_PER_UNIT, factor);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_SetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE factor, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%f, %lu)\n", iface, factor, apply);

    if(factor < DS3D_MINDOPPLERFACTOR ||
       factor > DS3D_MAXDOPPLERFACTOR)
    {
        WARN("Invalid parameter %f\n", factor);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.flDopplerFactor = factor;
        This->dirty.bit.dopplerfactor = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alDopplerFactor(factor);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_SetOrientation(IDirectSound3DListener *iface, D3DVALUE xFront, D3DVALUE yFront, D3DVALUE zFront, D3DVALUE xTop, D3DVALUE yTop, D3DVALUE zTop, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%f, %f, %f, %f, %f, %f, %lu)\n", iface, xFront, yFront, zFront, xTop, yTop, zTop, apply);

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.vOrientFront.x = xFront;
        This->params.vOrientFront.y = yFront;
        This->params.vOrientFront.z = zFront;
        This->params.vOrientTop.x = xTop;
        This->params.vOrientTop.y = yTop;
        This->params.vOrientTop.z = zTop;
        This->dirty.bit.orientation = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        ALfloat orient[6] = {
            xFront, yFront, -zFront,
            xTop, yTop, -zTop
        };
        setALContext(This->ctx);
        alListenerfv(AL_ORIENTATION, orient);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_SetPosition(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%f, %f, %f, %lu)\n", iface, x, y, z, apply);

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.vPosition.x = x;
        This->params.vPosition.y = y;
        This->params.vPosition.z = z;
        This->dirty.bit.pos = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alListener3f(AL_POSITION, x, y, -z);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_SetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE factor, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%f, %lu)\n", iface, factor, apply);

    if(factor < DS3D_MINROLLOFFFACTOR ||
       factor > DS3D_MAXROLLOFFFACTOR)
    {
        WARN("Invalid parameter %f\n", factor);
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(This->crst);
    if(apply == DS3D_DEFERRED)
    {
        This->params.flRolloffFactor = factor;
        This->dirty.bit.rollofffactor = 1;
    }
    else
    {
        struct DSBufferGroup *bufgroup = This->BufferGroups;
        DWORD i;

        setALContext(This->ctx);
        for(i = 0;i < This->NumBufferGroups;++i)
        {
            DWORD64 usemask = ~bufgroup[i].FreeBuffers;
            while(usemask)
            {
                int idx = CTZ64(usemask);
                DS8Buffer *buf = bufgroup[i].Buffers + idx;
                usemask &= ~(U64(1) << idx);

                if(buf->ds3dmode != DS3DMODE_DISABLE)
                    alSourcef(buf->source, AL_ROLLOFF_FACTOR, factor);
            }
        }
        checkALError();
        popALContext();

        This->rollofffactor = factor;
    }
    LeaveCriticalSection(This->crst);

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_SetVelocity(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%f, %f, %f, %lu)\n", iface, x, y, z, apply);

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params.vVelocity.x = x;
        This->params.vVelocity.y = y;
        This->params.vVelocity.z = z;
        This->dirty.bit.vel = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        setALContext(This->ctx);
        alListener3f(AL_VELOCITY, x, y, -z);
        checkALError();
        popALContext();
    }

    return S_OK;
}

static HRESULT WINAPI DS8Primary3D_SetAllParameters(IDirectSound3DListener *iface, const DS3DLISTENER *listen, DWORD apply)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);

    TRACE("(%p)->(%p, %lu)\n", iface, listen, apply);

    if(!listen || listen->dwSize < sizeof(*listen))
    {
        WARN("Invalid parameter %p %lu\n", listen, listen ? listen->dwSize : 0);
        return DSERR_INVALIDPARAM;
    }

    if(listen->flDistanceFactor > DS3D_MAXDISTANCEFACTOR ||
       listen->flDistanceFactor < DS3D_MINDISTANCEFACTOR)
    {
        WARN("Invalid distance factor (%f)\n", listen->flDistanceFactor);
        return DSERR_INVALIDPARAM;
    }

    if(listen->flDopplerFactor > DS3D_MAXDOPPLERFACTOR ||
       listen->flDopplerFactor < DS3D_MINDOPPLERFACTOR)
    {
        WARN("Invalid doppler factor (%f)\n", listen->flDopplerFactor);
        return DSERR_INVALIDPARAM;
    }

    if(listen->flRolloffFactor < DS3D_MINROLLOFFFACTOR ||
       listen->flRolloffFactor > DS3D_MAXROLLOFFFACTOR)
    {
        WARN("Invalid rolloff factor (%f)\n", listen->flRolloffFactor);
        return DSERR_INVALIDPARAM;
    }

    if(apply == DS3D_DEFERRED)
    {
        EnterCriticalSection(This->crst);
        This->params = *listen;
        This->params.dwSize = sizeof(This->params);
        This->dirty.bit.pos = 1;
        This->dirty.bit.vel = 1;
        This->dirty.bit.orientation = 1;
        This->dirty.bit.distancefactor = 1;
        This->dirty.bit.rollofffactor = 1;
        This->dirty.bit.dopplerfactor = 1;
        LeaveCriticalSection(This->crst);
    }
    else
    {
        union PrimaryParamFlags dirty = { 0l };
        dirty.bit.pos = 1;
        dirty.bit.vel = 1;
        dirty.bit.orientation = 1;
        dirty.bit.distancefactor = 1;
        dirty.bit.rollofffactor = 1;
        dirty.bit.dopplerfactor = 1;

        EnterCriticalSection(This->crst);
        setALContext(This->ctx);
        DS8Primary_SetParams(This, listen, dirty.flags);
        checkALError();
        popALContext();
        LeaveCriticalSection(This->crst);
    }

    return S_OK;
}

HRESULT WINAPI DS8Primary3D_CommitDeferredSettings(IDirectSound3DListener *iface)
{
    DS8Primary *This = impl_from_IDirectSound3DListener(iface);
    struct DSBufferGroup *bufgroup;
    LONG flags;
    DWORD i;

    EnterCriticalSection(This->crst);
    setALContext(This->ctx);
    This->DeferUpdates();

    if((flags=InterlockedExchange(&This->dirty.flags, 0)) != 0)
    {
        DS8Primary_SetParams(This, &This->params, flags);
        /* checkALError is here for debugging */
        checkALError();
    }
    TRACE("Dirty flags was: 0x%02lx\n", flags);

    bufgroup = This->BufferGroups;
    for(i = 0;i < This->NumBufferGroups;++i)
    {
        DWORD64 usemask = ~bufgroup[i].FreeBuffers;
        while(usemask)
        {
            int idx = CTZ64(usemask);
            DS8Buffer *buf = bufgroup[i].Buffers + idx;
            usemask &= ~(U64(1) << idx);

            if((flags=InterlockedExchange(&buf->dirty.flags, 0)) != 0)
                DS8Buffer_SetParams(buf, &buf->params, flags);
        }
    }
    checkALError();

    This->ProcessUpdates();
    popALContext();
    LeaveCriticalSection(This->crst);

    return DS_OK;
}

static const IDirectSound3DListenerVtbl DS8Primary3D_Vtbl =
{
    DS8Primary3D_QueryInterface,
    DS8Primary3D_AddRef,
    DS8Primary3D_Release,
    DS8Primary3D_GetAllParameters,
    DS8Primary3D_GetDistanceFactor,
    DS8Primary3D_GetDopplerFactor,
    DS8Primary3D_GetOrientation,
    DS8Primary3D_GetPosition,
    DS8Primary3D_GetRolloffFactor,
    DS8Primary3D_GetVelocity,
    DS8Primary3D_SetAllParameters,
    DS8Primary3D_SetDistanceFactor,
    DS8Primary3D_SetDopplerFactor,
    DS8Primary3D_SetOrientation,
    DS8Primary3D_SetPosition,
    DS8Primary3D_SetRolloffFactor,
    DS8Primary3D_SetVelocity,
    DS8Primary3D_CommitDeferredSettings
};


static HRESULT WINAPI DS8PrimaryProp_QueryInterface(IKsPropertySet *iface, REFIID riid, void **ppv)
{
    DS8Primary *This = impl_from_IKsPropertySet(iface);
    return DS8Primary_QueryInterface(&This->IDirectSoundBuffer_iface, riid, ppv);
}

static ULONG WINAPI DS8PrimaryProp_AddRef(IKsPropertySet *iface)
{
    DS8Primary *This = impl_from_IKsPropertySet(iface);
    LONG ret;

    ret = InterlockedIncrement(&This->prop_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static ULONG WINAPI DS8PrimaryProp_Release(IKsPropertySet *iface)
{
    DS8Primary *This = impl_from_IKsPropertySet(iface);
    LONG ret;

    ret = InterlockedDecrement(&This->prop_ref);
    TRACE("new refcount %ld\n", ret);

    return ret;
}

static HRESULT WINAPI DS8PrimaryProp_Get(IKsPropertySet *iface,
  REFGUID guidPropSet, ULONG dwPropID,
  LPVOID pInstanceData, ULONG cbInstanceData,
  LPVOID pPropData, ULONG cbPropData,
  ULONG *pcbReturned)
{
    (void)iface;
    (void)dwPropID;
    (void)pInstanceData;
    (void)cbInstanceData;
    (void)pPropData;
    (void)cbPropData;
    (void)pcbReturned;

    FIXME("Unhandled propset: %s\n", debugstr_guid(guidPropSet));

    return E_PROP_ID_UNSUPPORTED;
}

static HRESULT WINAPI DS8PrimaryProp_Set(IKsPropertySet *iface,
  REFGUID guidPropSet, ULONG dwPropID,
  LPVOID pInstanceData, ULONG cbInstanceData,
  LPVOID pPropData, ULONG cbPropData)
{
    (void)iface;
    (void)dwPropID;
    (void)pInstanceData;
    (void)cbInstanceData;
    (void)pPropData;
    (void)cbPropData;

    FIXME("Unhandled propset: %s\n", debugstr_guid(guidPropSet));

    return E_PROP_ID_UNSUPPORTED;
}

static HRESULT WINAPI DS8PrimaryProp_QuerySupport(IKsPropertySet *iface,
  REFGUID guidPropSet, ULONG dwPropID,
  ULONG *pTypeSupport)
{
    (void)iface;
    (void)dwPropID;
    (void)pTypeSupport;

    FIXME("Unhandled propset: %s\n", debugstr_guid(guidPropSet));

    return E_PROP_ID_UNSUPPORTED;
}

static const IKsPropertySetVtbl DS8PrimaryProp_Vtbl =
{
    DS8PrimaryProp_QueryInterface,
    DS8PrimaryProp_AddRef,
    DS8PrimaryProp_Release,
    DS8PrimaryProp_Get,
    DS8PrimaryProp_Set,
    DS8PrimaryProp_QuerySupport
};
