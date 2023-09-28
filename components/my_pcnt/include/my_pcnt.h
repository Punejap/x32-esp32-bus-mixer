#include "driver/pulse_cnt.h"

pcnt_unit_handle_t create_encoder(int high_limit, int low_limit, int GPIO_A, int GPIO_B );

void start_encoder(pcnt_unit_handle_t unit);