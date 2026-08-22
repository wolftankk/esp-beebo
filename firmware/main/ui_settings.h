#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void ui_settings_open(void);
void ui_settings_open_wifi(void);   /* straight to the network page */
void ui_settings_close_all(void);   /* hardware escape: back to the robot */
#ifdef __cplusplus
}
#endif
