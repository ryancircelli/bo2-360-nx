/*
 * Switch layer: VP8 decoder for the Zombies maps' loading videos (zm_*_load.webm).
 *
 * The game decodes them with its own libvpx, recompiled, which is too slow on the Switch; the app hands the
 * frames to this decoder instead (games/bo2/src/bo2zm_webm.cpp). VP8 is enabled along with what configure selects
 * for it: vp8_decoder_select = "h264pred videodsp vp8dsp". config.h includes it after each platform's header.
 */
#undef CONFIG_VP8_DECODER
#define CONFIG_VP8_DECODER 1
#undef CONFIG_VP8DSP
#define CONFIG_VP8DSP 1
#undef CONFIG_H264PRED
#define CONFIG_H264PRED 1
#undef CONFIG_VIDEODSP
#define CONFIG_VIDEODSP 1
