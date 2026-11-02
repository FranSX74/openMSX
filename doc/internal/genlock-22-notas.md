# openMSX 22 con GenLocker: estado y siguientes pasos

Notas de trabajo (2 de noviembre de 2026).

## Estado

- Base: etiqueta oficial `RELEASE_22_0` de openMSX.
- Funcionalidad: rama `feature/external-video-genlock` del fork
  `antxiko/openMSXgenlocker`, fusionada sin conflictos ni adaptaciones.
- Probado: NMS 8255 y NMS 8280 arrancan y `SET VIDEO 2 : COLOR ,0,0` superpone
  una imagen de prueba. La webcam y el panel External video input se probaron
  en la versión 21. El usuario dio por buena la instalación de la 22, sin que
  conste una prueba específica de la webcam en ella.
- Sin probar: capturadora real (no había ninguna conectada) y streams de red.

## Compilar

Dependencias en Arch/Omarchy además de las habituales: `tcl`, `glew` y, para
la captura, `ffmpeg`.

En `build/custom.mk` (cambio local, sin commit, porque depende del equipo):

```make
INSTALL_BASE:=/home/USUARIO/.local/openMSX
SYMLINK_FOR_BINARY:=false
```

Después: `./configure && make -j8 && make install`.

- `INSTALL_BASE` tiene que ser una ruta absoluta: el parser de openMSX no
  entiende `$(HOME)`.
- La ruta queda fijada dentro del binario, así que hay que definirla antes de
  compilar. Cambiarla solo en `make install` deja un binario que busca sus
  datos en otro sitio.
- Las ROMs van en `~/.openMSX/share/systemroms`.

## Uso

Arrancar `openmsx -machine Philips_NMS_8280`, abrir
**Settings > Video > External video input...**, elegir la fuente, pulsar
**Start** y activar la superposición en BASIC con `SET VIDEO 2 : COLOR ,0,0`.

Si el puente se lanza por fuera del panel, el vídeo llega, pero el panel no
reconoce la captura.

## Pendiente, en el orden propuesto

1. **Leer el manual de servicio del NMS 8280.** Aclara puertos F6/F7, registros
   y controles frontales. El enlace está en `external-video-genlock.md`.
2. **Audio externo.** Se puede seguir el modelo de `LaserdiscPlayer`, que es un
   `ResampledSoundDevice`. Hoy `SDLSoundDriver` solo abre audio de salida,
   `WavAudioInput` solo lee ficheros y el puente descarta el audio con `-an`.
   Un stream es el caso más sencillo para empezar, porque trae vídeo y audio
   juntos. Hay que compensar el retardo del vídeo.
3. **Primera versión del panel frontal.** Selector de fuente y niveles. El Tape
   Deck (`ImGuiMedia.cc`) usa widgets de ImGui e iconos dibujados por código.
4. **Digitalización a VRAM.** Empezar con imágenes fijas y comparar la VRAM con
   el resultado esperado. Al escribir en la memoria emulada, afecta al
   rebobinado y a las repeticiones: hay que guardar los fotogramas capturados o
   usar una fuente ligada al tiempo emulado, como el LaserDisc.
5. **Mejoras de sincronización.** Retardo, orden de campos y ajuste 50/60 Hz.
   El genlock analógico real no se puede reproducir, porque la capturadora
   entrega fotogramas digitales terminados.

## Detalles encontrados

- `SDLRasterizer.cc` tiene un `TODO` de superposición en `GRAPHIC5` (`SCREEN 6`).
- El panel pasa la URL a FFmpeg como `-- -i <URL>`, así que en principio también
  admite una ruta local (sin probar). Faltaría `-re` para que no vaya acelerado
  y un bucle al llegar al final.
- La superposición solo actúa sobre el color 0.
- `VDP::updateHostVideoFrame()` relee un PPM de unos 920 KB en cada fotograma
  mientras la fuente esté activa. Sustituirlo por memoria compartida es una
  posible mejora.
