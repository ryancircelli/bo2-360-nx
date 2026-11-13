// The campaign's addresses for the native renderer's game hooks (forced into the renderer's and the marker's sources
// from games/bo2/src by campaign/CMakeLists.txt, so those files build for this program unchanged).
//
// The renderer and bo2zm_d3d_marker.cpp name default_mp's functions; each define below turns a name into the
// campaign's function that does the same job, and the marker's return addresses into the campaign's. Every pair was
// matched the same way: instruction shapes equal apart from call targets and globals, the same callers and callees,
// and the call sites the return addresses follow.

#pragma once

// bo2zm_d3d_marker.cpp: the register-mirror dump helpers, the VS fetch patcher and the shader registrations.
#define sub_82174F38 sub_825B7F98            // register runs
#define __imp__sub_82174F38 __imp__sub_825B7F98
#define sub_82175178 sub_825B81D8            // fetch constants
#define __imp__sub_82175178 __imp__sub_825B81D8
#define sub_821752D0 sub_825B8330            // shader float constants
#define __imp__sub_821752D0 __imp__sub_825B8330
#define sub_82175420 sub_825B8480            // VS fetch patcher
#define __imp__sub_82175420 __imp__sub_825B8480
#define sub_82169BF8 sub_825AB790            // VS registration
#define __imp__sub_82169BF8 __imp__sub_825AB790
#define sub_821698D8 sub_825AAE38            // PS registration
#define __imp__sub_821698D8 __imp__sub_825AAE38

// bo2zm_d3d_marker.cpp: where the fetch patcher returns to, by caller.
#define BO2ZM_MARKER_BACK_PATCH_IN_ROOM 0x825B9050u  // default_mp 0x82176220 (sub_82176158 -> 825B8F88)
#define BO2ZM_MARKER_BACK_PATCH_ON_BIND 0x825B9B40u  // default_mp 0x82176D20 (sub_82176C18)
#define BO2ZM_MARKER_BACK_PATCH_COPY 0x825B8EB0u     // default_mp 0x82176080 (sub_82175F40 -> 825B8D80)

// The campaign shares sdmc:/switch/bo2zm with the Zombies program: its own shader library and pipeline cache.
#define BO2ZM_SHADER_LIBRARY_FILE "bo2cp_shaders.spvpack"
#define BO2ZM_NATIVE_CACHE_FOLDER "cache_cp"

// bo2zm_webm.cpp: the game's VP8 decoder replaced by FFmpeg for the .webm videos (logos, intro, loading screens).
// The same code in both programs, with the same decoder and frame-buffer offsets.
#define sub_824BC388 sub_823E0920            // receives each compressed frame
#define __imp__sub_824BC388 __imp__sub_823E0920
#define sub_824BC040 sub_823E05D8            // removes the decoder
#define __imp__sub_824BC040 __imp__sub_823E05D8
#define sub_8264C5F0 sub_82506EA0            // loop filter
#define __imp__sub_8264C5F0 __imp__sub_82506EA0
#define sub_8261EB58 sub_824D9A70            // 4x4 inverse DCT
#define __imp__sub_8261EB58 __imp__sub_824D9A70
#define sub_8261EAB8 sub_824D99D0            // DC-only add
#define __imp__sub_8261EAB8 __imp__sub_824D99D0
#define sub_82642288 sub_824FD150            // six-tap 16x16 (test hook)
#define __imp__sub_82642288 __imp__sub_824FD150
#define sub_826467E0 sub_825016B0            // six-tap 8x8 (test hook)
#define __imp__sub_826467E0 __imp__sub_825016B0
#define sub_824BFC78 sub_823E40E8            // decoder thread setup (test hook)
#define __imp__sub_824BFC78 __imp__sub_823E40E8

// bo2zm_waits.cpp and bo2zm_jobs.h: the job system's blocking waits; their numeric addresses come from BO2_GAME_ADDR
// (games/bo2/src/bo2zm_game.h).
#define BO2_GAME_CAMPAIGN 1
#define sub_8284AED8 sub_8262DF88            // picker
#define __imp__sub_8284AED8 __imp__sub_8262DF88
#define sub_827D8DD8 sub_825C15B0            // yield
#define __imp__sub_827D8DD8 __imp__sub_825C15B0
#define sub_8284A088 sub_8262D138            // worker wait
#define __imp__sub_8284A088 __imp__sub_8262D138
#define sub_8284A910 sub_8262D9C0            // push
#define __imp__sub_8284A910 __imp__sub_8262D9C0
#define sub_8284C3B8 sub_8262F468            // lock-free push
#define __imp__sub_8284C3B8 __imp__sub_8262F468
#define sub_828DED00 sub_826AEDC0            // culling wait
#define __imp__sub_828DED00 __imp__sub_826AEDC0
#define sub_8284AB18 sub_8262DBC8            // release list
#define __imp__sub_8284AB18 __imp__sub_8262DBC8
#define sub_8284AB88 sub_8262DC38            // release queue
#define __imp__sub_8284AB88 __imp__sub_8262DC38
#define sub_8284BB48 sub_8262EBF8            // help
#define __imp__sub_8284BB48 __imp__sub_8262EBF8
#define sub_8284B730 sub_8262E7E0            // wait for a job type (picker loop)
#define __imp__sub_8284B730 __imp__sub_8262E7E0
#define sub_824935E8 sub_823CA7F0            // the Backend's ring wait (help loop)
#define __imp__sub_824935E8 __imp__sub_823CA7F0
#define sub_82493548 sub_823CA750            // Main's wait for the Backend (help loop)
#define __imp__sub_82493548 __imp__sub_823CA750
#define sub_82493190 sub_823CA398            // wait for a job type
#define __imp__sub_82493190 __imp__sub_823CA398
#define sub_827D4BD8 sub_825C20C8            // WaitForSingleObject wrapper
#define __imp__sub_827D4BD8 __imp__sub_825C20C8
#define sub_82493690 sub_823CA898            // group wait
#define __imp__sub_82493690 __imp__sub_823CA898
#define sub_82493240 sub_823CA448            // help once with a job type
#define __imp__sub_82493240 __imp__sub_823CA448
#define sub_824931E8 sub_823CA3F0            // help once with any job
#define __imp__sub_824931E8 __imp__sub_823CA3F0
#define sub_82428ED8 sub_82380288            // Backend frame poll
#define __imp__sub_82428ED8 __imp__sub_82380288
#define sub_82428840 sub_8237FC00            // Main's frame-done poll
#define __imp__sub_82428840 __imp__sub_8237FC00
#define sub_828DA6F0 sub_826A6300            // ring-drained poll
#define __imp__sub_828DA6F0 __imp__sub_826A6300
#define sub_828DB270 sub_826A6E80            // device lock release
#define __imp__sub_828DB270 __imp__sub_826A6E80
#define sub_824A0BC0 sub_823D72B0            // leave critical section by index
#define __imp__sub_824A0BC0 __imp__sub_823D72B0
#define sub_828EFC68 sub_826C0B88            // try critical section 6
#define __imp__sub_828EFC68 __imp__sub_826C0B88
#define sub_82428878 sub_8237FC38            // set the frame event
#define __imp__sub_82428878 __imp__sub_8237FC38
#define sub_82429748 sub_82380A98            // set the frame event, hand over data
#define __imp__sub_82429748 __imp__sub_82380A98
#define sub_82429708 sub_82380A58            // set frame done
#define __imp__sub_82429708 __imp__sub_82380A58
#define sub_82442B90 sub_82335590            // sd_mix_voice
#define __imp__sub_82442B90 __imp__sub_82335590
#define sub_8243F3C8 sub_82331DF8            // sd_mix_master
#define __imp__sub_8243F3C8 __imp__sub_82331DF8
#define sub_8243F538 sub_82331F68            // sd_mix_radverb
#define __imp__sub_8243F538 __imp__sub_82331F68
#define sub_8258A368 sub_8249A0D8            // sd_voice_active
#define __imp__sub_8258A368 __imp__sub_8249A0D8
#define sub_8258A4C0 sub_8249A230            // sd_voice_free
#define __imp__sub_8258A4C0 __imp__sub_8249A230
#define sub_82428468 sub_8237F910            // WaitForSingleObject(*r3, INFINITE)
#define __imp__sub_82428468 __imp__sub_8237F910

// bo2zm_native_cull.cpp and bo2zm_native_entities.cpp: the bounds-tree culling and the entity scan in native code;
// their numeric addresses come from BO2_GAME_ADDR.
#define sub_828F6F08 sub_826D82C0            // bounds-tree culling
#define __imp__sub_828F6F08 __imp__sub_826D82C0
#define sub_828F6CB8 sub_826D8070            // culling: its state call
#define __imp__sub_828F6CB8 __imp__sub_826D8070
#define sub_828F6B78 sub_826D7F30            // culling: its item call
#define __imp__sub_828F6B78 __imp__sub_826D7F30
#define sub_822FD360 sub_8224AB18            // entity loop
#define __imp__sub_822FD360 __imp__sub_8224AB18
#define sub_822E6B78 sub_822357E0            // entity class job
#define __imp__sub_822E6B78 __imp__sub_822357E0
#define sub_822FE3B8 sub_8224BB00            // entity plane test
#define __imp__sub_822FE3B8 __imp__sub_8224BB00

// bo2zm_native_audio.cpp, bo2zm_native_limiter.cpp, bo2zm_native_lzx.cpp, bo2zm_native_skin.cpp: the sound mixer's hot code,
// the limiter, the fastfile decoder and CPU skinning in native code.
#define sub_8243F620 sub_82332050            // mono voice resampler
#define __imp__sub_8243F620 __imp__sub_82332050
#define sub_82440658 sub_82333088            // stereo voice resampler
#define __imp__sub_82440658 __imp__sub_82333088
#define sub_82589CC0 sub_82499A30            // sound pool
#define __imp__sub_82589CC0 __imp__sub_82499A30
#define sub_82589D48 sub_82499AB8            // sound pool
#define __imp__sub_82589D48 __imp__sub_82499AB8
#define sub_8258AD40 sub_8249AA90            // DSP helper
#define __imp__sub_8258AD40 __imp__sub_8249AA90
#define sub_8258AE90 sub_8249ABE0            // DSP helper
#define __imp__sub_8258AE90 __imp__sub_8249ABE0
#define sub_8258B0D0 sub_8249AE20            // DSP helper
#define __imp__sub_8258B0D0 __imp__sub_8249AE20
#define sub_8258B208 sub_8249AF58            // biquad
#define __imp__sub_8258B208 __imp__sub_8249AF58
#define sub_8258BE08 sub_8249BB58            // limiter
#define __imp__sub_8258BE08 __imp__sub_8249BB58
#define sub_8258C310 sub_8249C060            // reverb parameters (called)
#define __imp__sub_8258C310 __imp__sub_8249C060
#define sub_82592EE8 sub_824A2C90            // reverb setup (called)
#define __imp__sub_82592EE8 __imp__sub_824A2C90
#define sub_82593500 sub_824A3280            // reverb
#define __imp__sub_82593500 __imp__sub_824A3280
#define sub_827D3060 sub_825C0348            // block fill
#define __imp__sub_827D3060 __imp__sub_825C0348
#define sub_827E7058 sub_825CAB98            // fastfile stream decoder
#define __imp__sub_827E7058 __imp__sub_825CAB98
#define sub_82912D78 sub_826E5BF0            // skinning, one bone
#define __imp__sub_82912D78 __imp__sub_826E5BF0
#define sub_82913378 sub_826E61F0            // skinning, two bones
#define __imp__sub_82913378 __imp__sub_826E61F0
#define sub_82913B50 sub_826E69C8            // skinning, three bones
#define __imp__sub_82913B50 __imp__sub_826E69C8
#define sub_82914AA0 sub_826E7918            // skinning dispatcher
#define __imp__sub_82914AA0 __imp__sub_826E7918
#define sub_82594280 sub_824A3F98            // limiter helper (called)
#define __imp__sub_82594280 __imp__sub_824A3F98
#define sub_82594228 sub_824A3F40            // limiter helper (called)
#define __imp__sub_82594228 __imp__sub_824A3F40
#define sub_82918340 sub_826EA780            // pow (called)
#define __imp__sub_82918340 __imp__sub_826EA780

// bo2zm_preset.cpp, bo2zm_pad.cpp: the graphics presets. The bool registration and the
// per-frame call sit in bo2cp_autoexec.cpp (the campaign's render step, sub_822239E8).
#define sub_824DE0F8 sub_824068B0            // model version choice
#define __imp__sub_824DE0F8 __imp__sub_824068B0
#define sub_828A84F0 sub_826724C8            // shadows forced back on each frame
#define __imp__sub_828A84F0 __imp__sub_826724C8
#define sub_824985D0 sub_823CFEA8            // int dvar registration
#define __imp__sub_824985D0 __imp__sub_823CFEA8
#define sub_82498668 sub_823CFEF0            // float dvar registration
#define __imp__sub_82498668 __imp__sub_823CFEF0
#define sub_82498880 sub_823D0088            // enum dvar registration
#define __imp__sub_82498880 __imp__sub_823D0088
#define sub_827D8A48 sub_825C25A8            // XInputGetState (the button combination)
#define __imp__sub_827D8A48 __imp__sub_825C25A8

// bo2zm_ui.cpp, bo2zm_commands.cpp, bo2zm_switch.cpp: the menu Lua, the port's console commands and the audio engine's
// threads.
#define sub_824A0BA8 sub_823D7298            // enter the game's lock r3
#define __imp__sub_824A0BA8 __imp__sub_823D7298
#define sub_824A0BC0 sub_823D72B0            // leave it
#define __imp__sub_824A0BC0 __imp__sub_823D72B0
#define sub_82708D68 sub_82594F78            // compile a buffer (loadstring's)
#define __imp__sub_82708D68 __imp__sub_82594F78
#define sub_826F2720 sub_8257E9F0            // lua_pcall
#define __imp__sub_826F2720 __imp__sub_8257E9F0
#define sub_8270BDB0 sub_825316D0            // load a menu module by name
#define __imp__sub_8270BDB0 __imp__sub_825316D0
#define sub_8270BBD0 sub_82531508            // uncaught Lua error handler
#define __imp__sub_8270BBD0 __imp__sub_82531508
#define sub_82402E78 sub_82367328            // the console text core
#define __imp__sub_82402E78 __imp__sub_82367328
#define sub_8281A248 sub_825FDC50            // the audio engine's thread function
#define __imp__sub_8281A248 __imp__sub_825FDC50
#define sub_8281A2A8 sub_825FDCB0            // its second processing thread's function
#define __imp__sub_8281A2A8 __imp__sub_825FDCB0
