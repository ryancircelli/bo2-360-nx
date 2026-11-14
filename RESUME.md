WAIT runq 1113-225140-7564
b100 result: draw failures confirmed (tess_mode=1 flood, 13.6k pipeline fails) but log rotation (20 files x5MB) lost the start where 'native-xlate fallback (unsupported fetch opcode N)' is logged. b75k = 75s boot with --log_max_files=400 (run-boot-keep.sh); grep 'unsupported fetch opcode' across b75k/logs/bo1*.log. Then implement opcode in metal_msl_translator.cc.
