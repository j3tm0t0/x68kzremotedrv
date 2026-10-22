#include <stdint.h>
extern uint32_t fake_ms;
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return fake_ms; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)t; }
