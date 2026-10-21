# Warciv para Switch 1

Port homebrew con Trident como tileset predeterminado y las reglas actuales de
`classic-warclient`, incluida la IA largepox. El motor de la partida se enlaza
dentro de la misma aplicación: no hay que ejecutar un servidor en otra máquina
ni instalar un segundo programa en la consola.

Estado: el `.nro` compila con devkitA64/libnx. La validación de controles,
guardados, suspensión y funcionamiento en modo avión en una Switch física sigue
pendiente. Las pruebas Linux comprueban el motor integrado y su aislamiento;
no sustituyen esas pruebas en consola.

## Compilar

Desde la raíz del repositorio, con Docker disponible:

```sh
docker build -t warciv-switch-build -f platforms/switch/Dockerfile platforms/switch
docker run --rm -v "$PWD:/src" -w /src warciv-switch-build \
  python3 platforms/switch/build.py
```

La primera compilación descarga ICU 76.1, verifica su SHA-512 y genera las
herramientas nativas necesarias para compilarla para la consola. La imagen de
devkitPro está fijada por digest en el Dockerfile. Los archivos temporales
quedan en `build-switch`, `build-switch-host` y `build-switch-deps`.

El resultado es `dist-switch/switch/warciv/warciv.nro`. Incluye los datos del
juego en RomFS, Trident, las reglas de largepox y sus perfiles de configuración.

## Ejecutar

Copia `warciv.nro` a `/switch/warciv/warciv.nro` en la SD y ábrelo desde un
entorno homebrew ya disponible, preferentemente en modo aplicación con memoria
completa. Selecciona **Start New Game** para crear una partida local.

Los guardados se escriben en `sdmc:/switch/warciv/saves`, la configuración en
`sdmc:/switch/warciv` y el registro del motor en `sdmc:/switch/warciv/engine.log`.
Los datos opcionales de `sdmc:/switch/warciv/data` tienen prioridad sobre RomFS.

El perfil `switch-largepox.serv` carga el perfil competitivo existente y cambia
la orientación a mapa cenital, que corresponde a Trident. Conserva las reglas,
la IA, los recursos, el comercio y los ajustes de islas de largepox.

## Controles

| Control | Acción |
| --- | --- |
| Stick izquierdo | Mover el cursor |
| ZL + stick izquierdo | Mover el cursor con precisión |
| A | Clic principal; mantener para arrastrar |
| X | Clic secundario |
| B | Cancelar o cerrar |
| Y | Esperar con la unidad seleccionada |
| Cruceta | Mover la unidad, incluidas diagonales |
| Stick derecho | Desplazar el mapa |
| L / R | Alejar / acercar |
| + | Terminar turno |
| − | Guardado rápido durante una partida |
| Seleccionar un campo de texto | Abrir el teclado de la consola |
| Pantalla táctil | Interacción táctil del cliente SDL2 |

## Pruebas del motor

Se puede compilar el motor y el cliente para Linux dentro de la misma imagen:

```sh
docker run --rm -v "$PWD:/src" -w /src warciv-switch-build \
  meson setup build-embedded -Dclients=sdl2 -Dserver=enabled \
  -Dembedded-server=true -Ddefault_library=static -Dsyslua=false \
  '-Dfcdb=[]' '-Dfcmp=[]' '-Dtools=[]' -Daimodules=classic \
  -Dmwand=false -Dreadline=false -Dnls=false
docker run --rm --network none -v "$PWD:/src" -w /src warciv-switch-build \
  meson test -C build-embedded embedded-server-largepox --print-errorlogs
```

La prueba completa dos partidas con cuatro IA, comprueba que ambas terminan,
que el estado del cliente permanece intacto y que el guardado conserva
`classic-warclient`, `ailargepox`, el comercio Warclient y la geometría cenital.

## Arquitectura

El cliente SDL2 y el motor conservan copias independientes de libfreeciv y Lua.
`platforms/standalone/isolate_server.py` cambia los nombres de los símbolos del
motor y separa sus datos modificables. El motor se ejecuta en un hilo con pila
de 4 MiB; se detiene de forma cooperativa y se espera a que termine antes de
restaurar su estado inicial para una nueva partida. Sus llamadas a `exit()`
terminan ese hilo, no la aplicación.

El protocolo existente viaja exclusivamente por IPv4 de loopback
(`127.0.0.1`), con anuncios LAN y metaserver desactivados. Esto conserva la
separación de estados del cliente y el motor dentro de un único ejecutable.

## Validación pendiente en consola

- Arrancar, crear una partida y avanzar turnos con largepox y Trident.
- Guardar, cerrar la aplicación y cargar la partida desde la SD.
- Volver al menú y crear una segunda partida.
- Repetir con Wi-Fi desactivado o en modo avión.
- Probar Joy-Con, mando Pro, teclado, táctil, arrastre y zoom.
- Suspender y reanudar; alternar entre portátil y dock.
- Medir memoria y tiempos de IA conforme crece la partida.
