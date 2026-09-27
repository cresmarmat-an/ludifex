# Two defects in miniaudio 0.11.25 put clicks into world audio:
#
# - The gain smoother (ma_gainer) is wrong on its common paths: the stereo and
#   6-channel loops step two frames at a time but advance the gain by one
#   frame's worth, and the length of the ramp is computed with its operands
#   swapped, so a ramp covers the whole block at half speed. The next block
#   then starts from the full new gain. Every change to a spatialized sound's
#   gain (the sound moving, the listener turning) ends in a small step.
#
# - Seeking a sound leaves the input it had already read cached, so after a
#   seek up to a cache's worth of audio from the old position plays first and
#   the waveform then jumps to the new one. A voice that comes back from being
#   virtual is seeked to where it would have been, so it jumped.
#
# ludifex_patch_miniaudio writes a copy of miniaudio.h whose gainer ramps
# linearly over its smoothing time and then holds, and whose seek discards the
# stale cache, and returns the directory holding the copy. The fetched source
# is left untouched, so a program that also builds opane from the same
# checkout is unaffected. If a later miniaudio changes the code the patch
# anchors on, configuration stops with a message instead of building the
# unpatched code.

set(LudifexPatchMiniaudioFile "${CMAKE_CURRENT_LIST_FILE}")

function(ludifex_patch_miniaudio SourceDir OutputVariable)
    set(Source "${SourceDir}/miniaudio.h")
    set(PatchedDir "${CMAKE_CURRENT_BINARY_DIR}/miniaudio-patched")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Source}" "${LudifexPatchMiniaudioFile}")

    file(READ "${Source}" Header)

    set(Call "    return ma_gainer_process_pcm_frames_internal(pGainer, pFramesOut, pFramesIn, frameCount);")
    set(Definition "MA_API ma_result ma_gainer_process_pcm_frames(ma_gainer* pGainer, void* pFramesOut, const void* pFramesIn, ma_uint64 frameCount)\n{")
    set(Seek "        ma_node_set_time(pSound, seekTarget);\n")

    string(FIND "${Header}" "${Call}" CallAt)
    string(FIND "${Header}" "${Definition}" DefinitionAt)
    string(FIND "${Header}" "${Seek}" SeekAt)
    if(CallAt EQUAL -1 OR DefinitionAt EQUAL -1 OR SeekAt EQUAL -1)
        message(FATAL_ERROR
            "ludifex: miniaudio.h in ${SourceDir} no longer has the code that "
            "cmake/PatchMiniaudio.cmake corrects. Check whether this miniaudio version "
            "still ramps gains at half speed and keeps its cache across a seek, then "
            "update or remove the patch.")
    endif()

    set(Corrected [=[
/* Corrected by ludifex (cmake/PatchMiniaudio.cmake): ramps each channel's gain
   linearly from the old value to the new one over smoothTimeInFrames, then
   holds the new value. */
static ma_result ma_gainer_process_pcm_frames_corrected(ma_gainer* pGainer, void* pFramesOut, const void* pFramesIn, ma_uint64 frameCount)
{
    const ma_uint32 channels = pGainer->config.channels;
    const ma_uint32 smoothTime = pGainer->config.smoothTimeInFrames;
    ma_uint64 interpolatedFrameCount = 0;
    ma_uint64 iFrame;
    ma_uint32 iChannel;

    /* The original stays compiled, unused. */
    (void)&ma_gainer_process_pcm_frames_internal;

    if (pGainer->t < smoothTime) {
        interpolatedFrameCount = smoothTime - pGainer->t;
        if (interpolatedFrameCount > frameCount) {
            interpolatedFrameCount = frameCount;
        }
    }

    if (pFramesOut != NULL && pFramesIn != NULL) {
        float* pOut = (float*)pFramesOut;
        const float* pIn = (const float*)pFramesIn;

        if (interpolatedFrameCount > 0) {
            const float step = 1.0f / (float)smoothTime;
            for (iFrame = 0; iFrame < interpolatedFrameCount; iFrame += 1) {
                const float a = (float)(pGainer->t + iFrame) * step;
                for (iChannel = 0; iChannel < channels; iChannel += 1) {
                    const float gain = ma_mix_f32_fast(pGainer->pOldGains[iChannel], pGainer->pNewGains[iChannel], a);
                    pOut[iFrame*channels + iChannel] = pIn[iFrame*channels + iChannel] * gain * pGainer->masterVolume;
                }
            }
        }

        for (iFrame = interpolatedFrameCount; iFrame < frameCount; iFrame += 1) {
            for (iChannel = 0; iChannel < channels; iChannel += 1) {
                pOut[iFrame*channels + iChannel] = pIn[iFrame*channels + iChannel] * pGainer->pNewGains[iChannel] * pGainer->masterVolume;
            }
        }
    }

    if (pGainer->t == (ma_uint32)-1) {
        /* Frames have been processed, so later changes are interpolated. */
        pGainer->t = smoothTime;
    } else {
        pGainer->t = (ma_uint32)ma_min(pGainer->t + interpolatedFrameCount, smoothTime);
    }

    return MA_SUCCESS;
}

]=])

    string(REPLACE "${Call}" "    return ma_gainer_process_pcm_frames_corrected(pGainer, pFramesOut, pFramesIn, frameCount);" Header "${Header}")
    string(REPLACE "${Definition}" "${Corrected}${Definition}" Header "${Header}")
    string(REPLACE "${Seek}"
        "${Seek}\n        /* Corrected by ludifex: input cached before the seek is from the old position. */\n        pSound->processingCacheFramesRemaining = 0;\n"
        Header "${Header}")

    # Written through configure_file so the copy's timestamp only changes when
    # its content does, and reconfiguring does not rebuild the audio code.
    file(WRITE "${PatchedDir}/miniaudio.h.in" "${Header}")
    configure_file("${PatchedDir}/miniaudio.h.in" "${PatchedDir}/miniaudio.h" COPYONLY)

    set(${OutputVariable} "${PatchedDir}" PARENT_SCOPE)
endfunction()
