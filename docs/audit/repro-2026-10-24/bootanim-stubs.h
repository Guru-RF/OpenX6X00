/* The radio's libraries as bootanim/bootanim.c calls them, for its PC build (bootanim-host.c, #101): the CW key's
 * GPIO line (libgpiod 1) and the base's control library, both radios. run.py puts this header in their places. The
 * boot sound never starts on the PC (params.db is never opened), so none of them is called */
#include <stdbool.h>
#include <stdint.h>

struct gpiod_chip;
struct gpiod_line;
struct gpiod_chip *gpiod_chip_open_by_name(const char *name);
struct gpiod_line *gpiod_chip_get_line(struct gpiod_chip *chip, unsigned int offset);
int gpiod_line_request_output(struct gpiod_line *line, const char *consumer, int default_val);
void gpiod_line_release(struct gpiod_line *line);
void gpiod_chip_close(struct gpiod_chip *chip);

bool x6100_control_init(void);
void x6100_control_rxvol_set(uint8_t vol);
void x6100_control_record_set(bool on);
bool x6200_control_init(void);
void x6200_control_rxvol_set(uint8_t vol);
void x6200_control_record_set(bool on);
