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

/* Desactiva el device de arranque ("", cdrom0:, cdrom:) para assets.
   Se usa cuando opendir abre pero readdir no lista nada (HLE de emulador),
   ya que esos handles falsos envenenan las lecturas. */
void assets_disable_boot_device(void);

void assets_init(void);
void assets_shutdown(void);

#endif