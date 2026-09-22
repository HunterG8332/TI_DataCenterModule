#pragma once

#include <stdbool.h>

/* Board abstraction for the BMC. This header is the only place main.c
 * (or any other component) should learn about board wiring. Porting to
 * a new target (e.g. WROOM -> S3-ETH in Stage 7) means changing
 * bmc_board.c/.h and nothing else. */

void board_init(void);

void board_led_set(bool on);
