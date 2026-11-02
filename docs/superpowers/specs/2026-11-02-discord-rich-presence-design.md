# Discord Rich Presence: diseño

Fecha: 2026-11-02. Base: rama `test`, commit `0fbf924`.

## Objetivo

Mientras se juega, el perfil de Discord del jugador muestra en qué parte del juego está: lugar y región en el
exterior, santuario o Bestia Divina en los interiores, y la pantalla de título.

Es para uso personal del autor de este fork en Windows. No está pensado para enviarse al proyecto original.

Se considera terminado cuando, con el juego y Discord abiertos:

- el perfil muestra el lugar correcto en el exterior;
- al entrar y salir de un santuario, el estado cambia en menos de 10 segundos;
- con Discord cerrado, el juego se comporta y rinde igual que sin esta función.

## Fuera de alcance

- Soporte real en Linux y macOS (compilan, pero la función no hace nada).
- Botones, invitaciones o "unirse a la partida".
- Detectar el idioma de Discord: su conexión local no lo informa y leerlo
  por OAuth exige una clave secreta.
- Una opción en la pantalla de configuración.
- Mostrar menús de pausa, combate u objetivos de misiones.

## Qué se muestra

| Situación | Línea 1 (`details`) | Línea 2 (`state`) |
|---|---|---|
| Exterior, cerca de un lugar con nombre | Aldea Kakariko | Picos Gemelos |
| Exterior, lejos de un lugar con nombre | Explorando | Necluda |
| Santuario | Santuario de Tamuur | Interior de santuario |
| Bestia Divina | Vah Ruta | Bestia Divina |
| `FinalTrial` (DLC) | Prueba final | Interior |
| Pantalla de título | En el menú principal | (vacía) |

Todos los estados incluyen el tiempo de la sesión (`timestamps.start`, fijado una vez al iniciar el juego).

## Enfoque

La ubicación sale de dos fuentes que el runtime ya tiene, sin buscar nada nuevo en la memoria del juego:

1. **Posición de Link:** `link_position` en `runtime/control_game.c`, que lee `PlayerInfo +0x88`.
2. **Paquetes leídos:** el camino de lectura de `runtime/services/fs.c` conoce la ruta de cada archivo (lo usa
   `SAVAGEWIND_LOG_FILES`). Cada interior tiene su paquete: `Pack/DungeonNNN.pack`, `Pack/Remains*.pack`,
   `Pack/FinalTrial.pack`. La pantalla de título usa `Pack/Title.pack`.

Se descartó leer el mapa actual de la memoria (hay que encontrarlo en cada versión del juego) y leer
`savagewind.log` desde un programa aparte (responde tarde y agrega un proceso).

## Componentes

### `runtime/game_place.c` / `.h`

Decide dónde está el jugador. No conoce Discord.

- `game_place_on_read(const char *path)`: la llama `fs.c`. Solo compara prefijos de ruta y guarda el último escenario
  en una variable atómica, porque se ejecuta en cada lectura.
- `game_place_update(float x, float y, float z, bool has_link)`: calcula el texto actual.
- `game_place_lookup(...)`: función pura de búsqueda, sin estado, para poder probarla sola.

Estados: `TITLE`, `FIELD` e `INTERIOR` (con el nombre del paquete).

- Al leer un paquete de interior se pasa a `INTERIOR`.
- Al leer `Pack/Title.pack` se pasa a `TITLE`.
- Al leer `Map/MainField/`, `Physics/StaticCompound/MainField/`, `Physics/TeraMeshRigidBody/MainField/` o
  `NavMesh/MainField/` se vuelve a `FIELD`. Los archivos de `Map/` quedan en caché al entrar a un santuario; los demás se
  releen al salir.

### `runtime/game_place_table.inc`

Tabla generada por `make` desde `extracted/`. Contiene texto del juego, así que nunca se sube al repo (regla de
`CONTRIBUTING.md`) y está en `.gitignore`. Sin ella el juego compila igual, con textos genéricos en inglés. Contiene:

- lugares: nombre, coordenadas `x, z`, radio y si es un pueblo;
- regiones: los 15 nombres y una cuadrícula de 100 x 80 celdas sacada de `Ecosystem/MapTower.sbeco`;
- interiores: paquete y nombre.

### `tools/gen_place_table.py`

La llama `make` cuando falta la tabla o cambia la herramienta. Lee `Map/MainField/Static.smubin`, `Map/MainField/Location.smubin` y
`Ecosystem/MapTower.sbeco` desde `Pack/Bootup.pack`, y los textos de `Pack/Bootup_EUes.pack`, y escribe la tabla.
Reutiliza los lectores de SARC, Yaz0 y BYML de `tools/botw_save_convert.py`; el de MSBT va dentro de esta herramienta.
El juego no la necesita para ejecutarse. Los santuarios del DLC no tienen nombre en esos textos y se muestran como
"Santuario".

### `runtime/discord_rpc.c` / `.h`

Cliente del pipe local de Discord, solo Windows. Otras plataformas compilan versiones vacías de las mismas funciones.

- Prueba `\\.\pipe\discord-ipc-0` a `-9`.
- Envía el saludo (`op 0`, versión 1, `client_id`) y luego `SET_ACTIVITY` (`op 1`).
- Corre en un hilo propio. Si no hay conexión, reintenta cada 15 segundos.
- `discord_rpc_set(details, state)` solo copia los textos bajo un mutex; el hilo los revisa cada segundo.
- Al salir del juego se cierra el proceso y Discord limpia el estado solo.

### Enganches en código existente

- `fs.c`: una llamada a `game_place_on_read` donde se resuelve la ruta. Las lecturas del DLC (`aoc1`, santuarios
  120 a 136) pueden ir por otro camino: se engancha cada camino real.
- `control_place_frame` en `control_game.c`: cada 5 segundos llama a `game_place_update` y, si el texto cambió, a
  `discord_rpc_set`. Se separa de `SAVAGEWIND_PLACE_LOG` para que ese ajuste no apague la función.

## Reglas de ubicación

- **Lugar:** cada nombre tiene un radio según su tipo. Gana el más cercano en proporción a su radio, salvo dentro de un
  pueblo o del castillo, donde gana ese nombre. Sin ningún nombre en su radio, el texto es "Explorando".
- **Región:** la de la cuadrícula de torres del propio juego, con 100 unidades de precisión.
- **Sin Link:** si `has_link` es falso y el estado no es `TITLE`, se mantiene el último texto.

## Configuración

- `SAVAGEWIND_DISCORD=0` desactiva la función: no se crea el hilo.
- `SAVAGEWIND_DISCORD_LANG` fija el idioma del texto: `en`, `es` o la carpeta de un idioma del juego (`EUde`, `JPja`...).
  Sin ella se sigue el idioma del juego, en cualquiera de sus 16 idiomas, también si cambia con el juego abierto.
- Los nombres salen de los textos de cada idioma. Solo en inglés y español la región se recorta ("Lago"); en los demás
  se muestra el nombre completo de la torre. Los textos que el juego no tiene van escritos a mano en
  `tools/gen_place_table.py`.
- `DISCORD_APP_ID` es un `#define` en `discord_rpc.c`.
- `SAVAGEWIND_DISCORD_APP_ID` lo reemplaza sin recompilar. Sin ID válido, la función queda apagada y se registra en el
  log.

La aplicación de Discord la crea el jugador en el Developer Portal. La imagen grande es opcional: sin ella, Discord
muestra solo el texto.

## Errores

Ningún fallo detiene ni ralentiza el juego. Un pipe roto cierra la conexión y vuelve a reintentar. Cada cambio de
conexión deja una línea `[discord]` en el log, sin repetirse en cada reintento. Las comillas y barras de los nombres se
escapan al armar el JSON.

## Riesgos

- **Salida de interiores:** en una prueba manual `Map/MainField/` no se releyó al salir, por eso se usan también las
  rutas de física y navegación. Confirmado jugando.
- **Caché de paquetes:** volver al mismo santuario relee su paquete (confirmado jugando). Queda sin probar en Bestias Divinas y DLC.
- **`swgame.dll`:** la de `E:\extracted hom` no sirve para esta rama. Se regeneró en `build/`, sin tocar esa carpeta.

## Pruebas

- `tests/test_game_place.c`: búsqueda de lugar y región, límite del radio, transiciones de estado.
- `tests/test_discord_rpc.c`: formato de los mensajes y escape del JSON, sin conectarse a Discord.
- Prueba real: con Discord abierto, recorrer exterior, santuario y título, y comprobar el perfil. Repetirla con Discord
  cerrado y abriéndolo con el juego ya en marcha.
