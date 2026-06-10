#ifndef HC06_H_
#define HC06_H_

#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

bool hc06_check_connection();
bool hc06_set_name(char name[]);
bool hc06_set_pin(char pin[]);
bool hc06_set_baud_115200();
bool hc06_set_at_mode(int on);
bool hc06_config(char name[], char pin[]);

#ifdef __cplusplus
}
#endif

#endif // HC06_H_
