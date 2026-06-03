#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "perf.h"
#include "tensorflow/lite/micro/debug_log.h"
#include "tensorflow/lite/micro/micro_time.h"
#include "tensorflow/lite/micro/system_setup.h"

extern "C" void DebugLog(const char *, va_list) {}

extern "C" int DebugVsnprintf(char *buffer, size_t buf_size,
    const char *, va_list)
{
    if (buf_size != 0u && buffer != nullptr) {
        buffer[0] = '\0';
    }
    return 0;
}

namespace tflite {

void InitializeTarget() {}

uint32_t ticks_per_second()
{
    return 50000000u;
}

uint32_t GetCurrentTimeTicks()
{
    return read_mcycle_lo();
}

}  // namespace tflite
