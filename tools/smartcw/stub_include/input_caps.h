/* Host test shim (tools/smartcw): the test has no input devices */
#pragma once

#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

bool input_caps_bits(dev_t dev, const char *what, unsigned long *bits, size_t n);
bool input_caps_id(dev_t dev, struct input_id *id);
