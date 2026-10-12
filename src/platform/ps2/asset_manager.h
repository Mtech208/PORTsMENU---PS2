#ifndef ASSET_MANAGER_H
#define ASSET_MANAGER_H

#include <tamtypes.h>
#include "gs_renderer.h"

/* Carga (con cache LRU y downscale automatico) una imagen por ruta relativa.
   Devuelve la textura o NULL si no existe / VRAM llena. */
GsTexture *assets_get(const char *path);

/* Igual que assets_get pero marca la textura como permanente (nunca se evicta).
   Para el fondo global y otras texturas de uso continuo. */
GsTexture *assets_pin(const char *path);

void assets_init(void);
void assets_shutdown(void);

#endif