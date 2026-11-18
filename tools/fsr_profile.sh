cd "$(dirname "$0")/.."
export BENCH_RACENOW="drive=0" BENCH_PERF=1
B="REGPU_HYPER=off FM2_ANISOTROPY=16 FM2_GPU_PASSES=1"
T="FM2_RESOLUTION=1600x900 FM2_TEMPORAL_UPSCALERS=1 FM2_TEMPORAL_SCALE=80 FM2_UPSCALER=fsr3"
sh tools/hyper_bench.sh fp-native720 40 $B FM2_UPSCALER=off FM2_RESOLUTION=1280x720 >/dev/null
sh tools/hyper_bench.sh fp-native900 40 $B FM2_UPSCALER=off FM2_RESOLUTION=1600x900 >/dev/null
sh tools/hyper_bench.sh fp-fsr1 40 $B FM2_UPSCALER=fsr1 FM2_FSR_SCALE=80 FM2_RESOLUTION=1600x900 >/dev/null
sh tools/hyper_bench.sh fp-fsr2 40 $B $T FM2_FSR_VERSION=1 >/dev/null
sh tools/hyper_bench.sh fp-fsr3 40 $B $T FM2_FSR_VERSION=0 >/dev/null
sh tools/hyper_bench.sh fp-fsr3nomv 40 $B $T FM2_FSR_VERSION=0 FM2_NO_MOTION=1 >/dev/null
echo done
