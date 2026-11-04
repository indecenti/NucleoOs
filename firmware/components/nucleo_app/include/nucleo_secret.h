// nucleo_secret — small secrets (a password) an app keeps in the Cardputer's internal flash, never on the SD card,
// which anyone can take out and read on a PC. Each app sees only its own: the namespace is the foreground app's id.
// Values up to NUCLEO_SECRET_MAX - 1 bytes, names up to 15 characters. Not encrypted at rest unless the device has
// flash encryption on: it keeps a password off the removable card, it does not protect it from whoever holds the
// device. docs/native-apps.md ("Secrets").
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_SECRET_MAX 65

bool nucleo_secret_set(const char *name, const char *value);   // "" or NULL deletes it
bool nucleo_secret_get(const char *name, char *out, int cap);   // false = none (out is then "")

#ifdef __cplusplus
}
#endif
