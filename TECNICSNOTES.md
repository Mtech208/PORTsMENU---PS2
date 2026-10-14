# TECNICSNOTES - PORTsMENU PS2

Documentación técnica detallada de los diagnósticos, correcciones de renderizado a bajo nivel (GS), rediseño del layout visual y análisis del sistema dinámico de carga de juegos sin modificación de código.

**Cubre dos rondas de depuración**: la original (secciones 1-4: glitch de bandas, clipping por software, escaneo dinámico, menú de configuración) y la ronda de estabilización del carrusel Aurora (secciones 5-11: primitivas GS, límites DMA, heap del EE, ISO9660, quirks de newlib-PS2 y compatibilidad con ports).

---

## 1. Diagnóstico del Glitch Gráfico (Bandas Horizontales Estiradas)

### Síntoma
Al desplazarse por el carrusel horizontal hacia la derecha (seleccionando juegos avanzados como SM64 o PVZ), los juegos que salían por el extremo izquierdo de la pantalla generaban bandas horizontales de textura corrupta y estirada que atravesaban la mitad derecha de la pantalla.

### Causa Raíz a Nivel Hardware (PlayStation 2 GS)
El sintetizador gráfico (**Graphics Synthesizer - GS**) de la PS2 procesa primitivas mediante paquetes GIF dirigidos a registros internos:

1. **Formato de Coordenadas `XYZ2`:**
   - El registro `XYZ2` del GS almacena las coordenadas $(X, Y)$ en formato de **punto fijo 12.4 sin signo** (16 bits), con un rango representable de $0.0$ a $4095.9375$ píxeles.
   - La macro del SDK `GIF_SET_XYZ(x, y, z)` realiza una máscara directa de 16 bits: `((u64)(x) & 0xFFFF)`.

2. **Underflow por Coordenadas Negativas:**
   - En el código anterior de [`ui_menu.c`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c), la condición de descarte (cull) solo verificaba `anim_x < -140.0f` y permitía que `left` llegara hasta `-2000.0f`.
   - Cuando un cover salía por la izquierda, su coordenada `left` se volvía negativa (ej. $X = -20.0f$).
   - En punto fijo 12.4: $-20 \times 16 = -320$.
   - Al castearse a 16 bits sin signo: `-320 & 0xFFFF` = `0xFEC0` = **4076.0 píxeles**.

3. **Inversión y Estiramiento del Polígono:**
   - El vértice izquierdo ($V_1$) del cover quedaba asignado a $X = 4076$ (extremo derecho lejana del GS), mientras que el vértice derecho ($V_2$) quedaba en su posición real (ej. $X = 60$).
   - La unidad rasterizadora del GS intentaba trazar el strip conectando $X = 60$ con $X = 4076$, proyectando la textura completa a lo largo de todo el ancho del framebuffer recortado por scissor en 640 px, produciendo las franjas horizontales multicolor.

---

## 2. Correcciones Implementadas en el Código

### A. Capa de Renderizado ([`src/platform/ps2/gs_renderer.c`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/gs_renderer.c))

1. **Clipping de Texturas por Software (`gs_draw_sprite`):**
   - Para sprites axis-aligned (`angle == 0.0f`), se implementó recorte matemático exacto contra los límites de pantalla $[0, \text{SCREEN\_W}] \times [0, \text{SCREEN\_H}]$.
   - Si el sprite se sale parcialmente por la izquierda ($X < 0$), se recorta la distancia sobrante, se fija $X = 0.0f$ y se avanza la coordenada de textura $U$ proporcionalmente:
     $$\text{ratio} = \frac{-X}{\text{ancho\_original}}, \quad U = U + \text{ratio} \cdot UW, \quad UW = UW - \text{ratio} \cdot UW$$
   - Idéntico proceso para el borde derecho, superior e inferior.
   - Permite que los covers entren y salgan suavemente de la pantalla sin cortes bruscos y sin emitir coordenadas fuera de rango.

2. **Clipping de Primitivas Planas:**
   - [`gs_draw_rect`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/gs_renderer.c): Se añadió recorte estricto de coordenadas y dimensiones antes de emitir los vértices del sprite.
   - [`gs_draw_rounded_rect`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/gs_renderer.c): Si el rectángulo redondeado cruza los bordes de la pantalla, conmuta automáticamente a un rectángulo recortado limpio para evitar que los abanicos de triángulos (`TRIANGLEFAN`) generen vértices negativos.
   - [`gs_draw_rect_gradient`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/gs_renderer.c): El gradiente de reflejo en el piso ahora recorta sus bordes de forma segura.

3. **Blindaje en `make_xyz`:**
   - Se añadió un clamping defensivo a nivel de registro:
     ```c
     if (x < 0.0f) x = 0.0f;
     if (y < 0.0f) y = 0.0f;
     if (x > 2047.0f) x = 2047.0f;
     if (y > 2047.0f) y = 2047.0f;
     ```
   - Impide de forma absoluta cualquier underflow a enteros negativos de 16 bits.

4. **Corrección de `gs_clear`:**
   - Se corrigió el vértice final de `make_xyz(SCREEN_W << 4, SCREEN_H << 4, 0)` a `make_xyz(SCREEN_W, SCREEN_H, 0)` ya que `make_xyz` ya multiplica internamente por 16.

5. **Aislamiento de Texturas por Frame (`gs_frame_begin`):**
   - Se resetea `bound_tex = NULL` al inicio de cada frame para asegurar que cada nuevo paquete DMA emita siempre su registro `TEX0` correspondiente.

---

### B. Capa de Interfaz y Menú ([`src/platform/ps2/ui_menu.c`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c))

1. **Culling Estricto de Elementos:**
   - Se eliminó la tolerancia errónea `if (left < -2000.0f) return;`.
   - Se reemplazó por descarte de caja delimitadora completa:
     ```c
     if (left + tw <= 0.0f || left >= (float)SCREEN_W || floor_y <= 0.0f || top >= (float)SCREEN_H)
         return;
     ```

2. **Ajuste y Descenso de Posición (Layout):**
   - **Base del Carrusel:** Se bajó `CAR_FLOOR_Y` de `330` a `352` (+22 píxeles hacia abajo), asentando mejor los covers en la pantalla.
   - **Reflejo del Piso:** Se redujo la altura del degradado de `34 * scale` a `18 * scale` para un acabado más sutil y sin solapar textos.
   - **Título del Juego:** Bajado a `CAR_FLOOR_Y + 12` ($Y = 364$) centrado automáticamente con `draw_text_big_centered`.
   - **Subtítulo / Metadatos:** Se incorporó en $Y = 384$ una línea de metadatos dinámica que muestra el género/año del juego, o un tag por defecto `"PORT PS2"` en caso de no contar con archivo de configuración, eliminando el vacío inferior.
   - **Controles / Hints en Escala 2x:** Se reemplazó la línea diminuta de 8 px por dos líneas centradas en escala 2x (16 px por carácter):
     - Línea 1 ($Y = 404$): `X: JUGAR     < >: ELEGIR` (en blanco/celeste brillante).
     - Línea 2 ($Y = 424$): `L1/R1: RAPIDO    START: INICIO` (en gris plata).
   - **Distribución de Espaciado:** Espaciado regular y proporcional (~18-20 px entre elementos) desde el piso del carrusel hasta el borde inferior ($448$ px).

---

### C. Capa de Fuentes ([`src/platform/ps2/font8x8.c`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/font8x8.c))

- Se añadieron verificaciones de límites de pantalla en [`font_draw_glyph`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/font8x8.c#L115) y [`font_draw_glyph_scaled`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/font8x8.c#L127) para descartar caracteres fuera de la pantalla antes de emitir rectángulos al pipeline de dibujo.

---

## 3. Análisis: Añadir Juegos Nuevos sin Modificar Código

### ¿Se requiere modificar el código fuente para añadir juegos?
**NO. El sistema está diseñado para ser 100% dinámico y desacoplado del código C.**

### Mecanismo de Descubrimiento Autónomo:
El menú implementa un motor de auto-escaneo en tiempo de ejecución ([`scan_games`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c#L323)):

1. **Detección Automática de ELFs en `ELFS/`:**
   - Al iniciar, la función `scan_games()` abre el directorio `ELFS/` usando llamadas estándar POSIX de la PS2 (`opendir` / `readdir`).
   - Admite dos estructuras de carpetas indistintamente:
     - **Estructura por subcarpeta (Recomendada):** `ELFS/MIJUEGO/MIJUEGO.ELF` (o cualquier subnombre).
     - **Estructura directa:** `ELFS/MIJUEGO.ELF`.
   - Limpia el nombre del archivo (remueve la extensión `.ELF`, cambia guiones bajos `_` por espacios) y crea la entrada del juego automáticamente.

2. **Emparejamiento Automático de Arte (`ART/COV` y `ART/LOGO`):**
   - Previo al escaneo de ejecutables, `collect_art()` indexa todas las imágenes en `ART/COV/` y `ART/LOGO/`.
   - La función `match_art()` busca coincidencias sin importar:
     - Mayúsculas o minúsculas (`mijuego.jpg`, `MIJUEGO.PNG`, etc.).
     - Extensiones de imagen soportadas: `.JPG`, `.JPEG`, `.PNG`, `.BMP`.
     - Variaciones de nombre 8.3 generadas por ISO9660.

3. **Personalización Opcional mediante [`games.cfg`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/games.cfg):**
   - Si se desea personalizar el título en pantalla, cambiar la ruta del cover o añadir año/género/descripción, **no** se toca el código C. Solo se añade una línea en `games.cfg`:
     ```text
     Título en Pantalla|ruta/al/juego.elf|ruta/cover.jpg|ruta/logo.png||Género|Año|Descripción
     ```
   - Si no se especifica nada en `games.cfg`, el menú genera todos los valores de forma autónoma.

### Pasos para Añadir un Juego Nuevo:
1. Copiar el ejecutable del juego en:
   ```text
   ELFS/NOMBRE_JUEGO/NOMBRE_JUEGO.ELF
   ```
2. Copiar la portada en:
   ```text
   ART/COV/NOMBRE_JUEGO.JPG   (o .PNG / .BMP)
   ```
3. *(Opcional)* Copiar el logotipo en:
   ```text
   ART/LOGO/NOMBRE_JUEGO.PNG
   ```
4. Regenerar la ISO o ejecutar el launcher. El juego aparecerá inmediatamente en el carrusel sin necesidad de recompilar el proyecto.

---

## 4. Menú de Configuración, Idiomas y Reordenación de Juegos

Se implementó un sistema completo de configuración en tiempo de ejecución accesible mediante **`SELECT`** o **`TRIÁNGULO`**:

### A. Sistema Multilingüe (i18n)
- **Idiomas Soportados:**
  1. **ESPAÑOL** (Idioma por defecto)
  2. **ENGLISH**
  3. **PORTUGUÊS**
- **Traducción en Tiempo Real:** Al cambiar de idioma con `< >` en el menú de ajustes, todos los textos de la interfaz (hints de controles, subtítulos, nombres de opciones e indicaciones) se actualizan en vivo sin necesidad de reiniciar.

### B. Sistema de Ordenación y Reordenación de Juegos
El usuario puede decidir exactamente qué juego va primero, cuál va después y organizar la lista a su gusto:
1. **Modo Alfabético:**
   - `ALFABÉTICO (A-Z)`: Ordena todos los juegos de la A a la Z al instante.
   - `ALFABÉTICO (Z-A)`: Orden inverso.
2. **Modo Personalizado (Manual):**
   - **En el Menú de Configuración:** En la opción `MOVER JUEGO`, pulsando `< >` o `L1 / R1` se desplaza el juego seleccionado intercambiando posiciones con el anterior o siguiente en tiempo real.
   - **Directo en el Carrusel:** Se agregaron atajos directos con **`L2`** (mover juego a la izquierda) y **`R2`** (mover juego a la derecha), permitiendo organizar la biblioteca mientras se navegan las carátulas.
   - La transición visual de intercambio se anima suavemente en el carrusel mediante interpolación (`lerp`).

### C. Persistencia de Configuración (`portmenu.cfg`)
La configuración se guarda automáticamente al salir del menú de ajustes o al mover juegos:
- Guarda el idioma seleccionado (`lang=`).
- Guarda el modo de ordenación (`sort=`).
- Guarda el juego seleccionado (`selected=`).
- Guarda la lista ordenada de ejecutables (`game0=`, `game1=`, ...) para preservar el orden personalizado exacto del usuario entre reinicios.
- Compatible con múltiples medios: intenta guardar prioritariamente en Memory Card (`mc0:PORTMENU/portmenu.cfg`, `mc0:portmenu.cfg`), USB (`mass:portmenu.cfg`), PCSX2 host (`host:portmenu.cfg`) o localmente.

---

## 5. Primitivas del GS: TRIANGLE vs TRIANGLESTRIP y correspondencia UV↔vértice

### Síntoma original
Los covers se veían "en espejo"/diagonales: solo media textura dibujada en forma de triángulo.

### Causa
- El **primitive 3 del GS es `TRIANGLE`** (no strip): con 4 vértices emitidos, el GS dibuja **1 triángulo + 1 vértice huérfano** → media imagen diagonal.
- El primitive 4 (`TRIANGLESTRIP`) es el correcto para quads con UV.
- Además, en un strip el **orden UV↔posición debe ser TL, TR, BL, BR** (cada vértice arrastra su UV); un swap posiciones↔UVs produce el efecto espejo por mitades.

### Solución
- Texturizados: `TRIANGLESTRIP (4)` con 4 vértices TL→TR→BL→BR y UV correspondientes.
- Rects planos: `SPRITE (6)` con 2 vértices (más barato).
- Reflejos/marcos: `TRIANGLEFAN (5)` con **cierre explícito** del polígono (repetir el primer vértice del perímetro al final) — sin cierre, queda un hueco triangular en el lado izquierdo.

### Lección
En el GS cada primitive tiene aridad fija de vértices; mezclar aridad con N vértices emitidos no falla en el envío — **dibuja mal silenciosamente**.

---

## 6. DMA: límite QWC de 16 bits y subida de texturas por bandas

### Síntoma
Cargar el fondo a resolución completa (640×448×4 = 1.1 MB) colgaba la consola con TLB Miss / GIF atascado.

### Causa
El campo **QWC del DMA es de 16 bits (máx 65,535 qwords)**. Una textura de 1.1 MB requiere 71,680 qwords → overflow del registro → transferencia corrupta.

### Solución (banded upload en `gs_texture_create`)
- Dividir la subida en **bandas de filas** tales que cada transferencia ≤ 16,000 qwords:
  `rows_max = (16000 * 16) / (pw * 4)`
- Cada banda emite su propio `BITBLTBUF / TRXPOS(dsay=y) / TRXREG / TRXDIR / IMAGE tag / TEXFLUSH`.
- Efecto secundario positivo: menos presión de heap por buffer temporal.

### VRAM y cache
- VRAM allocator con **free-list first-fit** (evita el bump allocator que fragmenta al evictar).
- Cache LRU de texturas con **pin** (fondo global nunca se evicta) y flag **failed** (assets inexistentes no se reintentan cada frame — evita saturación de `open` en el log y I/O).

---

## 7. Heap del EE (32 MB): TLB Miss en memcpy/memset hacia 0x2000000

### Síntoma
`TLB Miss pc=0x11cxxxx addr=0x2000000 [store]` en ráfaga, escribiendo secuencialmente byte a byte desde 0x2000000 (exactamente 1 byte después de los 32 MB de RAM). Con `addr2line`: `memcpy` de kernel.S (que en el tail copia byte a byte).

### Causa
`stbi_load` decodifica **a resolución completa del archivo** antes de cualquier downscale propio. Portadas de 1024×1500+ generan temporales de 6-12 MB; sumado a covers residentes y fragmentación, el buffer toca el borde del heap (0x2000000) y el decode/resample escribe pasando el límite.

### Solución
1. **Gate con `stbi_info`** (lee dimensiones sin decodificar): rechazar imágenes > 2048×2048 **antes** del `stbi_load`.
2. **Downscale por tipo de asset** con resize proporcional exacto (no por mitades, que bajaba el fondo 640×448 → 320×224 borroso):
   - `ART/BG/` → máx **640×448** (resolución nativa PS2, nítido)
   - `ART/COV/` → máx **320×480** (portrait)
   - `ART/LOGO/` → máx **320×96**
3. **NULL-checks con log** en los `memalign` del texture upload (`HEAP agotado...`), para que un fallo de memoria sea diagnosticable y no un TLB Miss silencioso.

---

## 8. ISO9660 para PS2: path tables y el "open fail" del IOP

### Síntoma
La ISO montaba en PCSX2 y `cdvdLoadElf` encontraba el ELF, pero el **loadelf del IOP** fallaba con `open fail name PORTMENU.ELF;1` → boot sin cargar → cascada de TLB Miss.

### Causa (3 bugs en cadena del generador mkiso)
1. **Root de la path table**: debe ser un identificador de **1 byte con valor 0x00** y parent `1`. Se estaba generando con nombre vacío/`_` y parent `0`.
2. **Números de directorio**: el parent de cada entrada debe ser el **número secuencial del directorio en la path table** (1-based). Usar el índice del árbol plano (donde los archivos desplazan los índices) apuntaba a entradas inexistentes (ej. parent 18 de 15 entradas) → `cdvdman` no puede resolver ninguna ruta.
3. **pt_size desalineado** con el padding real de los records (el root mide 10 bytes con pad, no 8).

### Otras leyes del formato
- Nombres 8.3 **uppercase** estrictos; archivos con `;1`, directorios sin versión.
- Extent de la **M path table impar** (convención), L par.
- El cdvdman del IOP **sí usa las path tables** aunque PCSX2 (ISOReader propio) tolere su ausencia/corrupción — por eso "PCSX2 encuentra el ELF pero el open falla".

### Lección de tooling
- `Remove-Item -Recurse` de PowerShell **atraviesa junctions** y borró `ELFS/` y `ART/` reales. Para limpiar un staging con junctions: `[IO.Directory]::Delete($junction)` (solo el link) antes del borrado recursivo.
- Si PCSX2 tiene la ISO montada, `fopen(..., "wb")` falla (archivo bloqueado) → generar a nombre alterno y renombrar.

---

## 9. Quirks de newlib-PS2 (EE)

1. **`clock()`**: inestable en newlib-PS2 → para throttles usar `frame_count` interno (contador de frames), nunca `clock()`/timers.
2. **`gs_log` con throttle por frames**: máximo 1 write al archivo de log cada 15 frames; los omitidos se agrupan en `[log] N mensajes omitidos`. Imposible saturar el log desde el menú.
3. **`padRead`**: retorna 0 = sin datos este frame (no es error). Los botones son **active-low**: `pressed = ~status.btns`.
4. **printf cortado**: un TLB storm puede cortar un `printf` a mitad de línea en la consola de PCSX2 — no confiar en el output visible para diagnóstico; usar `PORTMENU.log`.
5. **cdrom en PS2 Real y PCSX2**: El driver `CDVDMAN.IRX` del BIOS de Sony **no implementa `dopen` ni `dread`** (enumeración de directorios) para `cdrom0:`. Tanto en PS2 real como en el BIOS de PCSX2, `opendir` devuelve handles mudos (`nulldev`) y `readdir` devuelve 0 entradas. Por tanto, **en disco CD/DVD nunca se debe depender de `opendir` en runtime**: los juegos deben leerse directamente por lista pre-indexada (`GAMES.CFG`), la cual `make_iso.ps1` autogenera en el PC durante el build. Jamás se debe desactivar el device de arranque al detectar 0 entradas en un scan.

---

## 10. Compatibilidad con ports de la escena

1. **`argv[0]` = ruta del ELF**: los ports (SMK64, SSB64, OptiCraft, PVZ) derivan de ahí la carpeta de sus datos (`main.pak`, `assets.pak`, texturepacks). El launcher siempre pasa la ruta completa como argumento.
2. **Rutas hardcodeadas**: algunos ports buscan rutas fijas del disco original (SMK64 busca `\N64\MK64.BIN;1`). Solución: replicar esa estructura dentro de la ISO (carpeta `N64/`) además de la copia junto al ELF.
3. **PANIC logs de los ports**: los ports imprimen su propio panel de error con la ruta exacta buscada — es la fuente más rápida para diagnosticar assets faltantes.
4. **`LoadExecPS2` con device implícito**: anteponer `cdrom0:` a rutas sin device antes de lanzar.

---

## 11. Matching difuso de covers y gestión de nombres

- El scan recolecta los nombres reales de `ART/COV/` y `ART/LOGO/` (uppercase) y matchea contra el base del ELF: **exacto → prefijo común ≥ 3 caracteres** (tolera truncado 8.3 de ISO9660: `OPTICRAFT.elf` ↔ `OPTICRAF.JPG`).
- El matching difuso también aplica al override de `games.cfg`: una ruta referida con nombre largo matchea el ELF truncado de la ISO.
- La detección de images muertas (flag `failed` por ruta) evita reintentos de `open` por frame que saturen el log y el I/O.

---

## 12. Incidencia de Compilación: Identificador no declarado en tabla i18n (`txt_port_ps2`)

### Error reportado por GCC 15.2.0 (cross-compiler EE)
```text
C:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c: In function 'ui_render':
C:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c:1074:48: error: 'txt_port_ps2' undeclared (first use in this function)
 1074 |             snprintf(meta, sizeof(meta), "%s", txt_port_ps2[lang]);
      |                                                ^~~~~~~~~~~~
```

### Causa
Al implementar el soporte multilingüe (Español, Inglés y Portugués), todas las cadenas de texto del menú se parametrizaron en arreglos constantes indexados por el idioma actual (`lang`). Al actualizar la función de renderizado [`ui_render()`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c#L1023) para mostrar el subtítulo dinámico traducido en lugar de un string literal `"PORT PS2"`, se referenció el identificador `txt_port_ps2[lang]` sin haber incluido su definición en la tabla estática de cadenas al inicio de `ui_menu.c`.

### Solución aplicada
Se incorporó la declaración e inicialización correspondiente en el bloque de traducciones de [`src/platform/ps2/ui_menu.c`](file:///c:/Users/gamer/OneDrive/Desktop/PORTsMENUPS2/src/platform/ps2/ui_menu.c#L85-L87):

```c
static const char *txt_port_ps2[LANG_COUNT] = {
    "PORT PS2", "PS2 PORT", "PORT PS2"
};
```

Con esta corrección, el compilador enlaza correctamente la referencia idiomática y el build con Ninja/CMake finaliza limpiamente.

---

## 13. PS2 real: la ISO grabada en CD **no bootea directo** sin Modchip/Exploit

### Síntoma
La ISO grabada en CD-R sin chip o sin exploit no bootea por protección ATIP/Wobble de Sony. En cambio, si la consola tiene **Modchip** (Matrix, Modbo) o se usa **Swap Magic**, el CD-R sí es leído como disco de PlayStation 2.

---

## 14. Diagnóstico de Pantalla Negra tras el Boot Splash en PS2 Real

### Síntoma
Al arrancar el CD en PS2 real, el disco carga, muestra brevemente la pantalla de arranque ("BOOT splash"), y de inmediato la pantalla queda completamente oscura / colgada. En PCSX2, en cambio, entraba al menú.

### Causas Raíz Detectadas en el Código

1. **Bucle infinito en `padInit` / Modelos Slim**:
   - `pad_init()` solo intentaba cargar `rom0:SIO2MAN` y `rom0:PADMAN`.
   - En consolas **PS2 Slim** (SCPH-75000 a 90000), esos módulos no existen con ese nombre; Sony los renombró a `rom0:XSIO2MAN` y `rom0:XPADMAN`.
   - Al fallar la carga, `padInit(0)` entraba en un bucle infinito en el EE esperando el RPC bind del IOP (`[EE] Skipping timeout loop at 0x001010E8`). PCSX2 detecta y salta estos bucles por HLE, pero en hardware real la consola se congela indefinidamente.
   - Además, `pad_init()` se llamaba *después* del splash bloqueando la transición al menú.

2. **Rutas de `cdrom0:` con forward-slash (`/`)**:
   - El driver `CDVDMAN` de Sony en el IOP **solo reconoce barras invertidas (`\`)** (formato Windows/DOS) en `cdrom0:`, ej. `cdrom0:\ART\BG\BOOT.JPG;1`.
   - Si se pasan barras `/`, el driver las concatena sin separador o devuelve error de archivo no encontrado. En PCSX2 funcionaba porque su capa HLE corregía las rutas o caía a `host:`.

3. **`games.cfg` vacío y dependencia de `opendir`**:
   - El archivo `games.cfg` en el repositorio estaba sin entradas esperando que `scan_games()` escaneara `ELFS/` con `opendir`.
   - Dado que `cdrom0:` no soporta `opendir`, el menú encontraba **0 juegos**, la pantalla se limpiaba a negro/azul muy oscuro y mostraba pantalla de espera de diagnóstico vacía.
   - La cascada de escaneo fallido mutaba `media_prefix` hasta `hdd0:/`.

4. **Desactivación indebida de `cdrom0:` para texturas**:
   - El antiguo detector de "HLE falso" ejecutaba `assets_disable_boot_device()` al ver que `readdir` devolvía 0 entradas, lo que bloqueaba permanentemente todas las lecturas de covers, fondos y logos desde `cdrom0:`.

### Soluciones Implementadas

1. **`pad_init` universal y no bloqueante**:
   - Soporte automático para Fat y Slim: intenta `SIO2MAN` / `PADMAN` y hace fallback a `XSIO2MAN` / `XPADMAN`.
   - Se inicia de inmediato al arrancar el ELF, permitiendo que el handshake del DualShock ocurra en segundo plano durante el splash sin retrasos.
   - `pad_read()` verifica que el estado sea `PAD_STATE_STABLE` antes de leer el puerto.

2. **Generación automática de `GAMES.CFG` en el ISO (`make_iso.ps1`)**:
   - Durante la compilación de la ISO en Windows, PowerShell escanea `ELFS/` y `ART/` e indexa todos los juegos con títulos, rutas completas, covers y metadatos dentro de `GAMES.CFG`.
   - Así, el usuario **nunca tiene que modificar código C ni editar archivos manualmente** al añadir juegos: basta poner la carpeta en `ELFS/` y correr `make_iso.ps1`.
   - Al arrancar en PS2 real, `PORTMENU` abre `GAMES.CFG` con `fopen` directo (que sí funciona 100% en `cdrom0:`) y carga todos los títulos de inmediato.

3. **Normalización estricta de rutas con `\` y soporte de sufijo `;1`**:
   - Toda lectura dirigida a `cdrom0:` convierte automáticamente cualquier `/` a `\`.
   - Si un archivo no abre, se intenta automáticamente añadiendo el descriptor de versión ISO9660 `;1`.

4. **Eliminación del bloqueo de dispositivo de arranque**:
   - `assets_disable_boot_device()` se convirtió en no-op para que el CD-ROM nunca sea silenciado en hardware real.

---

## 15. Diagnóstico de Pantalla Blanca en Open PS2 Loader (OPL)

### Síntoma
Al arrancar la ISO en OPL desde USB (o red), el cargador avanza por los colores de depuración y se congela permanentemente en **Pantalla Blanca**.

### Significado del color Blanco en OPL
En OPL, los colores de depuración trazan la ejecución:
- **Naranja / Rojo**: Búsqueda del juego e inicialización de medios.
- **Verde**: Parcheo del kernel y hooks de IGR.
- **Azul / Morado**: Limpieza de memoria y sincronización del IOP.
- **Blanco**: **Intento de transferir el control y ejecutar el ELF principal (`BOOT2` de `SYSTEM.CNF`)**.

Si OPL se congela en **Blanco**, significa que la inicialización previa fue exitosa, pero **no pudo cargar o ejecutar el ELF principal**.

### Causas y Soluciones

1. **Falta de Game ID en `SYSTEM.CNF`**:
   - OPL requiere obligatoriamente que `BOOT2` apunte a un Game ID estándar con formato `XXXX_###.##` (ej. `SLUS_200.01`).
   - Anteriormente apuntaba a `PORTMENU.ELF;1`, lo que impedía a OPL reconocer el juego y cargar el ejecutable.
   - **Solución**: Ahora `SYSTEM.CNF` especifica `BOOT2 = cdrom0:\SLUS_200.01;1`, el archivo `SLUS_200.01` existe en la raíz de la ISO y la ISO para USB se genera como `SLUS_200.01.PORTMENU.iso`.

2. **Ubicación incorrecta en la USB (`CD/` vs `DVD/`)**:
   - Dado que la ISO pesa ~339 MB y está estructurada como CD-ROM, debe colocarse en la carpeta **`CD/`** de la memoria USB (`USB:\CD\SLUS_200.01.PORTMENU.iso`).
   - Si se coloca en `DVD/`, OPL espera una estructura DVD y falla con pantalla blanca.

3. **Fragmentación del archivo en la USB (Causa más frecuente en OPL USB)**:
   - Si la memoria USB FAT32 tiene fragmentos, OPL no puede leer secuencialmente los sectores del juego y se congela en blanco.
   - **Solución**: Desfragmentar la USB con herramientas como **Defraggler** o **OPL Manager**, o formatear la USB y copiar el archivo en limpio.

4. **Alternativa recomendada: Ejecución directa como APP en OPL**:
   - No es obligatorio usar una ISO en USB: se puede copiar `PORTMENU.elf` directamente a la USB junto con `ART/`, `ELFS/` y `games.cfg`, y lanzarlo desde el menú **APPS** de OPL o con **uLaunchELF** sin necesidad de emular discos.

---

## 16. Diagnóstico: "ELF no encontrado" en OptiCraft/Pou y bucle de carga en Plants vs Zombies

### Síntomas observados
1. Al seleccionar **OptiCraft** o **Pou**, el menú mostraba en pantalla `"ELF no encontrado: cdrom0:\..."` y no arrancaba.
2. Al arrancar **Plants vs Zombies (PVZ)**, el ejecutable iniciaba pero entraba en un bucle infinito en emulador y consola real repitiendo:
   `open name cdfs:/main.pak flag 1`
   `Bad Sector Count Error`
   `***** FILE /main.pak CAN NOT FOUND ******`

### Causas identificadas tras análisis de logs y binarios

1. **Restricción 8.3 de ISO9660 en `mkiso.c` vs Rutas en `games.cfg` (OptiCraft)**:
   - El sistema de archivos ISO9660 Nivel 1 utilizado para CD de PS2 limita los nombres de directorio y el cuerpo de los archivos a un máximo de **8 caracteres**.
   - La carpeta y archivo original de Minecraft tenían 9 letras: `OPTICRAFT` (O-P-T-I-C-R-A-F-T).
   - Al empaquetar la ISO, `mkiso.c` recortó automáticamente `OPTICRAFT` a `OPTICRAF` (8 letras).
   - Sin embargo, `games.cfg` seguía apuntando a `ELFS/OPTICRAFT/OPTICRAFT.elf`.
   - Cuando el menú intentaba abrir `cdrom0:\ELFS\OPTICRAFT\OPTICRAFT.elf`, el lector de CD reportaba que el archivo no existía (`open fd = -1`).

2. **Sensibilidad a mayúsculas/minúsculas en el TOC del CD (Pou)**:
   - En `games.cfg` y en disco, el ELF de Pou estaba en minúsculas (`pou_ps2.elf`).
   - En la norma ISO9660, todos los nombres son almacenados en estricto **uppercase** (`POU_PS2.ELF`).
   - El driver CDVDMAN de PS2 fallaba al intentar resolver nombres en minúsculas con `fopen`.

3. **Rutas fijas a la raíz del disco en los Ports Homebrew (Plants vs Zombies y OptiCraft)**:
   - **Plants vs Zombies**: El motor del port tiene hardcodeada la búsqueda de su paquete de recursos principal en `cdfs:/main.pak` (la raíz del CD-ROM), ignorando subcarpetas.
   - **OptiCraft**: Busca `assets.pak` y los módulos IOP de sonido y red (`DATA/IRX/`, `skins/`, `texturepacks/`) directamente en la raíz de `cdrom0:/`.

### Soluciones implementadas

1. **Estandarización 8.3 en mayúsculas para todos los juegos**:
   - `ELFS/OPTICRAFT/OPTICRAFT.elf` renombrado a `ELFS/OPTICRAF/OPTICRAF.ELF`.
   - `ELFS/POU/pou_ps2.elf` renombrado a `ELFS/POU/POU_PS2.ELF`.
   - `games.cfg` actualizado con las rutas normalizadas.

2. **Resolución inteligente de rutas en el Launcher (`ui_menu.c`)**:
   - Se implementó `resolve_elf_path()` que prueba automáticamente combinaciones:
     - Separadores `\` y `/`.
     - Variantes en mayúsculas (`toupper`).
     - Sufijo de versión ISO9660 `;1`.
     - Truncado automático de componentes a 8.3 si el usuario introduce rutas largas.

3. **Vinculación de recursos en la raíz de la ISO (`make_iso.ps1`)**:
   - Se colocaron en la raíz del disco: `MAIN.PAK` (PVZ), `ASSETS.PAK` (OptiCraft), `data/` (IRXs), `skins/` y `texturepacks/`.

4. **Deduplicación de Extents en `mkiso.c`**:
   - `mkiso.c` ahora detecta archivos idénticos en distintas rutas de la imagen y les asigna el mismo extent (LBA) físico en el disco ISO.
   - Gracias a esto, `MAIN.PAK` y `ASSETS.PAK` existen tanto en la raíz como dentro de `ELFS/` sin ocupar un solo megabyte duplicado, manteniendo la ISO en ~339.8 MB (apta para CD de 700 MB).

---

## 17. Corrección Crítica: Pérdida de Sincronía en CRT ("VIDEO 1") y Lanzamiento Universal de Ports

### Síntoma
Al seleccionar **OptiCraft (Minecraft)** o cualquier otro port desde el menú en una consola conectada a un televisor CRT (tubo), la pantalla se iba a negro y el televisor mostraba el cartel emergente de entrada **"VIDEO 1"**, indicando pérdida total de sincronismo de la señal analógica.

### Causa Raíz
1. **Reset destructivo del Graphics Synthesizer en `gs_shutdown()`:**
   - El código anterior ejecutaba `*GS_REG_CSR = (u64)1 << 9;` al iniciar el lanzamiento.
   - El bit 9 del registro CSR del GS reinicia el hardware de video y apaga los generadores de temporización y reloj (PCRTC).
   - Esto cortaba inmediatamente la señal analógica NTSC/PAL (0 voltios en H-Sync/V-Sync), haciendo que el CRT perdiera la sincronización y mostrara su mensaje de canal ("VIDEO 1").
2. **Conflicto de rutas en `LoadExecPS2` vs argumentos del juego hijo (`argv[0]`):**
   - El BIOS de Sony (`rom0:EELOAD`) requiere estrictamente el descriptor de versión ISO9660 `;1` para abrir un ELF en `cdrom0:` (ej. `cdrom0:\ELFS\OPTICRAF\OPTICRAF.ELF;1`).
   - Sin embargo, si ese mismo string con `;1` se le pasaba al juego en `argv[0]`, las funciones internas del juego (`dirname`, `strrchr(argv[0], '/')`) construían rutas corruptas como `cdrom0:\ELFS\OPTICRAF\OPTICRAF.ELF;1/assets.pak`.
3. **Pérdida de separador en candidatos 8.3:**
   - `snprintf(candidates[ncand], "%s%s", prefix, out_rest)` generaba `cdrom0:ELFS\...` omitiendo la barra inclinada tras los dos puntos.

### Soluciones Aplicadas
1. **Mantener la señal analógica viva en el GS:**
   - Se eliminó el reset por hardware `1 << 9` de CSR.
   - `gs_shutdown()` ahora emite un frame negro limpio con `gs_clear(black)`, espera el VSync y apaga únicamente el canal DMA del GIF. La señal de video se mantiene continua y el televisor CRT nunca pierde la sincronía.
2. **Separación estricta de `kernel_path` y `argv[0]`:**
   - `LoadExecPS2` recibe `kernel_path` garantizando el descriptor `;1` en `cdrom0:` para que la BIOS encuentre el ejecutable.
   - El arreglo `args[0]` que recibe la aplicación hija se limpia de `;1` para que sus lecturas relativas funcionen al 100%.
3. **Staging 100% dinámico en `make_iso.ps1`:**
   - Se eliminaron las reglas fijas por nombre de juego. El script escanea dinámicamente cualquier subcarpeta en `ELFS/`, vincula automáticamente archivos `.pak` y subdirectorios de datos (`data/`, `skins/`, `images/`, etc.) a la raíz del ISO, deduplicando sectores con `mkiso.c`.
   - Se pueden añadir nuevos ports sin tocar una sola línea de código C ni scripts.
