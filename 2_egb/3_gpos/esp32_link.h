#ifndef _ESP32_LINK_H_
#define _ESP32_LINK_H_

/* Constantes compartidas entre el driver y las aplicaciones de usuario.
 * Solo macros, sin includes del kernel, para que lo puedan incluir los
 * dos lados. */

#define ESP32_DEV_CMD   "/dev/esp32link_cmd"
#define ESP32_DEV_DATA  "/dev/esp32link_data"
#define MAX_LINE  128

#endif
