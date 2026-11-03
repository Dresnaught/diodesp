#ifndef LOVYAN_CONFIG_H
#define LOVYAN_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ili9488_t;

ili9488_t get_ili9488(void);
void GF_to_LV(void);

#ifdef __cplusplus
}
#endif

#endif // LOVYAN_CONFIG_H