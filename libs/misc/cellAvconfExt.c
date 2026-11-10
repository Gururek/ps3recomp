/*
 * ps3recomp - cellAvconfExt HLE implementation
 *
 * Reports standard LPCM stereo 48kHz as available audio output.
 * Games query this to decide audio format before opening cellAudio ports.
 */

#include "cellAvconfExt.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static float s_gamma = 1.0f;
static CellAudioOutConfiguration s_audio_configuration = {
    CELL_AUDIO_OUT_CHNUM_2, CELL_AUDIO_OUT_CODING_TYPE_LPCM, { 0 }, 0
};

static s32 audio_out_validate(u32 audioOut)
{
    if (audioOut == CELL_AUDIO_OUT_PRIMARY)
        return CELL_OK;
    if (audioOut == CELL_AUDIO_OUT_SECONDARY)
        return (s32)CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    return (s32)CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option)
{
    (void)audioOut;
    (void)option;

    /* Report the SAME capability set a real PS3/RPCS3 reports, not just stereo.
     * Golden trace (RPCS3.log, real YDKJ boot) shows:
     *   cellAudioOut: found support for Linear PCM 2 Ch. / 5.1 Ch. / 7.1 Ch.
     *   cellAudioOut: found support for Dolby Digital 5.1 Ch. / DTS 5.1 Ch.
     * This call returns the MAX channel count available for (type, fs). Under-reporting
     * stereo (the old behaviour) makes FMOD's init probe fail its 8/6-channel checks and
     * then fall through to DTS/AC3 probes that also returned 0 -- YDKJ ends up with no
     * FMOD threads at all. Returning the real 7.1 for LPCM matches the console. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM) {
        if (fs & CELL_AUDIO_OUT_FS_48KHZ) return CELL_AUDIO_OUT_CHNUM_6;  /* 5.1 (A/B: was 8) */
        return CELL_AUDIO_OUT_CHNUM_2;
    }

    /* Dolby Digital / DTS: real hardware reports 5.1 for these. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_AC3 || type == CELL_AUDIO_OUT_CODING_TYPE_DTS)
        return CELL_AUDIO_OUT_CHNUM_6;

    return 0; /* not available */
}

s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option)
{
    (void)option;

    if (audioOut != CELL_AUDIO_OUT_PRIMARY)
        return 0;
    if (!(fs & CELL_AUDIO_OUT_FS_48KHZ))
        return 0;
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
        (ch == CELL_AUDIO_OUT_CHNUM_2 || ch == CELL_AUDIO_OUT_CHNUM_6 ||
         ch == CELL_AUDIO_OUT_CHNUM_8))
        return (s32)ch;
    if ((type == CELL_AUDIO_OUT_CODING_TYPE_AC3 || type == CELL_AUDIO_OUT_CODING_TYPE_DTS) &&
        ch == CELL_AUDIO_OUT_CHNUM_6)
        return (s32)ch;

    return 0;
}

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info)
{
    printf("[cellAvconfExt] AudioOutGetDeviceInfo(out=%u, dev=%u)\n",
           audioOut, deviceIndex);

    if (!info)
        return (s32)CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
    s32 rc = audio_out_validate(audioOut);
    if (rc != CELL_OK)
        return rc;
    if (deviceIndex != 0)
        return (s32)CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE;

    memset(info, 0, sizeof(CellAudioOutDeviceInfo));

    /* Report the mode set a real PS3 reports. Golden trace (RPCS3.log, real YDKJ boot):
     *   "found support for Linear PCM 2 Ch. / 5.1 Ch. / 7.1 Ch."
     *   "found support for Dolby Digital 5.1 Ch. / DTS 5.1 Ch."
     * Advertising a single stereo mode here while GetSoundAvailability reports 5.1/7.1 is
     * self-inconsistent, and middleware (FMOD) validates these against each other. */
    info->portType = 0; /* HDMI */
    info->state = 1;    /* available */
    info->latency = 13;
    info->availableModeCount = 5;
    info->availableModes[0].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[0].channel = CELL_AUDIO_OUT_CHNUM_2;
    info->availableModes[0].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[0].layout  = CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH;
    info->availableModes[1].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[1].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[1].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[1].layout  = CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr;
    info->availableModes[2].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[2].channel = CELL_AUDIO_OUT_CHNUM_8;
    info->availableModes[2].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[2].layout  = CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy;
    info->availableModes[3].type    = CELL_AUDIO_OUT_CODING_TYPE_AC3;
    info->availableModes[3].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[3].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[3].layout  = CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr;
    info->availableModes[4].type    = CELL_AUDIO_OUT_CODING_TYPE_DTS;
    info->availableModes[4].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[4].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[4].layout  = CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr;

    return CELL_OK;
}

s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize)
{
    (void)option;
    (void)optionSize;

    printf("[cellAvconfExt] AudioOutGetConfiguration()\n");

    if (!config)
        return (s32)CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
    s32 rc = audio_out_validate(audioOut);
    if (rc != CELL_OK)
        return rc;

    *config = s_audio_configuration;

    return CELL_OK;
}

s32 cellAudioOutSetCopyControl(u32 audioOut, u32 control)
{
    (void)audioOut;
    printf("[cellAvconfExt] AudioOutSetCopyControl(control=%u)\n", control);
    return CELL_OK;
}

s32 cellAudioOutGetNumberOfDevice(u32 audioOut)
{
    if (audioOut == CELL_AUDIO_OUT_PRIMARY)
        return 1;
    if (audioOut == CELL_AUDIO_OUT_SECONDARY)
        return 0;
    return (s32)CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
}

s32 cellVideoOutGetGamma(u32 videoOut, float* gamma)
{
    (void)videoOut;
    if (!gamma) return CELL_EINVAL;
    *gamma = s_gamma;
    return CELL_OK;
}

s32 cellVideoOutSetGamma(u32 videoOut, float gamma)
{
    (void)videoOut;
    printf("[cellAvconfExt] VideoOutSetGamma(%.2f)\n", gamma);
    s_gamma = gamma;
    return CELL_OK;
}

/* cellAudioOutConfigure(audioOut, config, option, waitForEvent) -- set the
 * selected host-visible output mode. NID 0x4692AB35. */
s32 cellAudioOutConfigure(u32 audioOut, CellAudioOutConfiguration* config,
                          void* option, u32 waitForEvent)
{
    (void)option;
    (void)waitForEvent;
    if (!config)
        return (s32)CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
    s32 rc = audio_out_validate(audioOut);
    if (rc != CELL_OK)
        return rc;

    const int channel_supported = config->channel == CELL_AUDIO_OUT_CHNUM_2 ||
        config->channel == CELL_AUDIO_OUT_CHNUM_6 ||
        config->channel == CELL_AUDIO_OUT_CHNUM_8;
    const int encoder_supported = config->encoder == CELL_AUDIO_OUT_CODING_TYPE_LPCM ||
        config->encoder == CELL_AUDIO_OUT_CODING_TYPE_AC3 ||
        config->encoder == CELL_AUDIO_OUT_CODING_TYPE_DTS;
    const int encoding_combo_supported = config->encoder == CELL_AUDIO_OUT_CODING_TYPE_LPCM ||
        config->channel == CELL_AUDIO_OUT_CHNUM_6;
    if (!channel_supported || !encoder_supported || !encoding_combo_supported)
        return (s32)CELL_AUDIO_OUT_ERROR_UNSUPPORTED_SOUND_MODE;

    s_audio_configuration.channel = config->channel;
    s_audio_configuration.encoder = config->encoder;
    if (config->downMixer <= 2)
        s_audio_configuration.downMixer = config->downMixer;
    memset(s_audio_configuration.reserved, 0, sizeof(s_audio_configuration.reserved));
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u, channel=%u, encoder=%u)\n",
           audioOut, config->channel, config->encoder);
    return CELL_OK;
}
