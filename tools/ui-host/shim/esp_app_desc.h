#pragma once
typedef struct { char version[32]; char project_name[32]; char time[16]; char date[16]; char idf_ver[32]; } esp_app_desc_t;
#ifdef __cplusplus
extern "C"
#endif
const esp_app_desc_t *esp_app_get_description(void);
