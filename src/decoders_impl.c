/* Third-party single-file decoder implementations, compiled once and kept
 * out of the warning flags used for our own code. */
#define DR_MP3_IMPLEMENTATION
#include "../third_party/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
