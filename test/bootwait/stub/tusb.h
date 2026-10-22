#include <stdint.h>
#include <stdbool.h>
#include <string.h>
enum { SCSI_SENSE_NOT_READY = 2, SCSI_SENSE_MEDIUM_ERROR = 3, SCSI_SENSE_ILLEGAL_REQUEST = 5 };
enum { SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL = 0x1e };
typedef struct { uint8_t b[18]; } scsi_sense_fixed_resp_t;
static inline bool tud_msc_set_sense(uint8_t l, uint8_t k, uint8_t a, uint8_t q) { return true; }
