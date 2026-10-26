# Savage Wind

Savage Wind (repositorio *Savage Wind Recomp*) es una recompilación estática de *Breath of the Wild* (Switch)
para Windows y Linux. Parte de LASRecomp beta 3 (MIT), la recompilación estática de *Link's Awakening*.

Notas de desarrollo. El README explica qué hace falta y las reglas de copyright para quien llega al repositorio.

## Reglas legales del proyecto

- El repositorio y cualquier build no contienen archivos del juego, código del juego (tampoco el C
  generado), assets, fuentes del sistema ni keys.
- Savage Wind no desencripta nada. La entrada son archivos ya extraídos por el usuario desde su propia
  consola y su propia copia del juego, en `extracted/` (ignorado por git).
- La recompilación corre en la máquina del usuario (`make gen` o la pantalla de setup), igual que en
  LASRecomp beta 3.
- No se usan marcas de Nintendo en el nombre, los íconos ni los binarios del proyecto.
- `.gitignore` bloquea `dump/`, `keys/`, `*.keys`, `*.xci`, `*.nsp`, `*.nca` y similares.

## Publicación

GitHub: `Revivedx/SavageWindRecomp` (remote `origin`, público desde el 2026-10-16). No se sube el historial
local: la importación de LASRecomp trae capturas de *Link's Awakening* y su randomizer. La rama local `public`
lleva un commit por publicación con el árbol de `main` y el email noreply de GitHub. Desde el 2026-10-21 se sube a
la rama remota `test`: el `main` remoto tiene un ruleset ("main": sin borrado, sin force push, cambios por pull
request y "Restrict updates" sin bypass), así que entra a `main` con un pull request de `test` (#1 para Alpha 2),
que solo se puede fusionar si el dueño del repo cambia ese ruleset:

```
export GIT_AUTHOR_NAME=Revivedx GIT_AUTHOR_EMAIL=82332536+Revivedx@users.noreply.github.com
export GIT_COMMITTER_NAME=$GIT_AUTHOR_NAME GIT_COMMITTER_EMAIL=$GIT_AUTHOR_EMAIL
git branch -f public $(git commit-tree "main^{tree}" -p public -m "<qué cambia>")
git -c credential.helper= -c "credential.helper=!gh auth git-credential" push origin public:test
```

Antes de cada publicación: `git ls-tree -r --name-only main` no debe tener archivos del juego, código generado,
keys ni rutas personales.

Releases: el zip de `make dist` (ver `docs/BUILD-windows.md`) se sube con `gh release create v<versión>
--prerelease` sobre el commit de `public`. Alpha 1 (`0.1.0-alpha.1`, 2026-10-17) se probó como jugador: zip
extraído en una carpeta nueva, PATH sin MSYS2, preparación desde la pantalla de inicio (copia, `romfs.bin`,
generación y compilación con swgen + zig: 2308 s con 12 hilos) y PLAY hasta tener a Link en el Santuario, a 30
fps. La carpeta queda en ~30,5 GB (romfs 15 GB, `romfs.bin` 15,1 GB, `swgame.dll` 209 MB), ~32 GB durante la
compilación. Para la prueba sirve `SAVAGEWIND_LAUNCHER_AUTOPLAY=1` con `SAVAGEWIND_HOST_INPUT` (la tecla E es A)
y `SAVAGEWIND_SCREENSHOT_EVERY` (capturas en `build/` de la carpeta de trabajo).

Desde el 2026-10-18 `savagewind.exe` entra directo al juego si los archivos están preparados y `swgame` coincide con
este build; si no, muestra solo la preparación (modo setup) y al terminar arranca el juego. `--config` (lo que hace
`Savage Wind Configuration.exe`, runtime/platform/windows_config.c; en Linux la acción "Savage Wind Configuration"
del `.desktop` del AppImage; en macOS mantener Opción al abrir la app) muestra la pantalla de configuración completa;
"Quit to Start Screen" vuelve a ella. Las pruebas de la pantalla (`SAVAGEWIND_LAUNCHER_SHOT`, `_SETUP`, `_EXTRACT`,
`_DELETE`) la muestran siempre, en modo configuración, o en modo setup con `SAVAGEWIND_LAUNCHER_MODE=setup`. El fondo
(runtime/ui/splash.mm) es arte propio dibujado (un claro de bosque con la espada del icono en una piedra), o `splash.jpg` / `splash.png` del
jugador en la carpeta de datos (nunca en el repo ni en el zip).

Antes de copiar nada, la preparación comprueba el RomFS elegido contra la versión del `exefs/main`, con la columna de
ficheros del RomFS de `runtime/game_versions.h` (los que tiene con la actualización aplicada; 1.8.2: 41.056; 0: sin
recuento). El RomFS debe decir esa versión en `System/Version.txt` y tener al menos el 95 % de esos ficheros: así se
detectan el RomFS del cartucho sin la actualización (dice 1.0.0) y el RomFS de la actualización sola (muchos menos
ficheros).

## Estructura de entrada esperada

```
savage_wind/extracted/
  exefs/   main, main.npdm, rtld, sdk (y subsdk* si el juego los trae)
  romfs/   contenido completo de la romfs (base + actualización ya fusionadas)
```

La versión principal y la única con soporte es la 1.9.0 (2026-02-17, v1114112: texto en tailandés en Switch 2,
SDK 21.4.1), la última: un dump legal hecho hoy la trae. La 1.8.2 (versión de título v1048576) se sigue instalando y
funciona, pero sin soporte: un issue de 1.8.2 se arregla solo si también pasa en la 1.9.0 (decisión del usuario,
2026-10-22; ver "Versiones del juego"). El build id del módulo `main` identifica
la versión exacta; `tools/sw_probe.py` lo imprime.

## Cambios respecto de LASRecomp

Hechos:

- `tools/patches.py`: vacío. Los parches por instrucción de LASRecomp apuntaban a direcciones de
  Link's Awakening y habrían roto (o corrompido) la generación de BOTW.
- `tools/gen.py`: ya no carga `tools/rando/patch_variants.json` (sitios del randomizer de LA). El
  descubrimiento de funciones quedó en `discover()` para poder reusarlo desde otras herramientas.
- `tools/sw_probe.py`: nuevo. Mide el exefs sin escribir C (ver abajo).
- `.gitattributes`: finales de línea LF en checkouts de Windows.
- Runtime sin lo exclusivo de Link's Awakening (verificado con `make runtime-objs` en MSYS2 UCRT64:
  sin errores ni warnings, y el `.exe` enlaza):
  - Borrados: `runtime/rando.[ch]`; `runtime/ui/` `rando_tab`, `spoiler_view`, `tracker_overlay`,
    `inventory_overlay`, `pets_ui`, `chaos_overlay`, `save_editor`, `warp_tab`, `difficulty`, `cheats`,
    `shader_tour` (recorría los puntos de warp de LA) y sus datos `warp_data.h` / `item_names.h`;
    `runtime/hle/` `chamber`, `digs`, `doorjump`, `escape`, `flags_ext`, `hookshot`, `keysanity`,
    `miniboss`, `pets`, `pot_glow`, `qol`, `rescue`, `shopsanity`, `souls`, `warp`, `widescreen`;
    `runtime/nvn/texpatch.[ch]` (retoques de texturas de la UI de LA: iconos de botones Xbox/PlayStation,
    icono de Hero Mode, color de túnica y corazones, botella dorada).
  - Borrados fuera del runtime: `third_party/LAS-Randomizer`, `tools/rando/`, `tools/warp_table.py`,
    los objetivos `rando-helper` y `smoketest` del Makefile, `tools/linux/smoketest.sh`,
    `tools/doorjump_test.sh` y `tools/perf_sweep.py` (dependían del warp y del randomizer), keystone en
    `tools/build_deps_macos.sh`, el venv del randomizer en `tools/setup_windows.ps1`, y las capturas de
    LA de `docs/` (`ultrawide.jpg`, `start_screen.jpg`).
  - `make gen-helper` usa su propio venv (`build/gen-venv`: capstone, unicorn, lz4, PyInstaller);
    `tools/linux/rando_helper.sh` pasó a `tools/linux/gen_helper.sh` y solo congela el generador.
  - `tools/gen.py`: fuera el camino muerto de `rando_word()` (`RANDO_CODE` siempre estaba vacío).
  - Archivos genéricos revisados: `main.c` (sin seeds; `SAVAGEWIND_REBOOT_AT=<frame>:<game|title>`),
    `services/fs.c` (el overlay de la RomFS queda como herramienta de desarrollo:
    `SAVAGEWIND_ROMFS_OVERLAY=<carpeta>` reemplaza o agrega archivos; las partidas van siempre a `save/`),
    `hle/input.c` (sin ticks de LA; las variables de prueba que vivían en archivos de LA pasaron acá),
    backends Vulkan y Metal (sin pot/pets/widescreen/texpatch; la imagen queda siempre en 16:9 con
    barras), `settings.[ch]` (solo ajustes genéricos: video, audio, `dpad_move`, aviso del menú),
    `ui/menu.mm` (pestañas Controls, Video, Audio y Quit), `ui/launcher.mm` (sin selector de seeds,
    sin pregunta de aspecto, sin consulta de actualizaciones a GitHub de LASRecomp, sin fondo del video
    de título de LA; acepta cualquier `main` que sea NSO y muestra su build id). `paths.c`,
    `gamebuild.c`, `romfs_build.c`, `fnprof.c`, `hle/framepace.c` y `nvn/cmdbuf.c` no tenían ganchos
    de LA aparte de los nombres.
- Nombres: `savagewind[.exe]`, `savagewind.dll.a`, `swgame.{dll,so,dylib}` (entrada `swgame_tables`),
  generador congelado `swgen` (`tools/swgen.spec`), `savagewind.ini`, `savagewind.log`,
  `local.savagewind` (macOS), `~/.local/share/savagewind` (Linux), variables de entorno
  `SAVAGEWIND_*` (antes `LARECOMP_*`, incluida `SAVAGEWIND_DATA_ROOT`), "Savage Wind" en la UI.
- Módulos en memoria (era el pendiente "ajustar `MAIN_BASE`/`SDK_BASE` y cargar `subsdk*`"): ya no hay
  direcciones fijas por módulo; las calcula `tools/layout.py` y se cargan todos los `subsdk*`. Ver abajo.
- Arranque con el `nn::init::Start` de 1.8.2 (`_ZN2nn4init5StartEmmPFvvES2_S2_`, tres callbacks; el de
  LA tenía dos). Desensamblado, hace: `_init_libc0`, `nnosInitialize`, **x2**, `_init_libc1`,
  `nninitInitializeSdkModule`, `nninitStartup`, `_init_libc2`, **x3**, `nnMain`, **x4**,
  `nninitFinalizeSdkModule`, `nnosQuickExit`. `runtime/main.c` busca primero esa firma (si no, la de dos)
  y pasa x2 = inicializar `sdk`, x3 = inicializar `subsdk9`..`0` y `main`, x4 = finalizarlos en orden de
  carga (`DT_FINI`, o `fini_array` al revés); los destructores del `sdk` no corren (la salida es rápida).
  Para llegar a x2 hicieron falta además `DT_RELR` (abajo) y `svcGetInfo` 28 (`AliasRegionExtraSize`, 0).
- Jump tables en `tools/gen.py` (era el pendiente de `printf_core`, ver abajo).
  - Antes, el descubrimiento de funciones tomaba como función nueva el destino de un `adr` hecho dentro de la
    misma función. Ese destino es la base de una jump table: 611 en el `sdk`, 953 en `main` y 334 en `subsdk0`.
    Así partía la función, y los cortes se encadenaban: `printf_core` quedaba en 38 pedazos. El `br` caía en
    un pedazo sin sus casos, y el `switch` de respaldo solo cubre el pedazo: "no function at 0x700292070c".
  - Ahora el rango de cada función sale de su FDE de `.eh_frame` (`analyze.eh_frame_extents`, el
    `pc_range`). Un `adr`/`adrp`+`add` a una dirección de la misma función es una etiqueta, no un inicio.
  - El resolver busca la guarda, la base y los casos en toda esa función, no solo en el pedazo. Emula la
    ventana en línea recta: los `cbz`/`tbz` que el compilador intercala siguen de largo, y el `cmp` puede
    tener `cbz`/`tbz` entre él y la guarda. Una lectura por un registro sin valor lee ceros. La guarda puede
    estar hasta 32 instrucciones antes del `br`. Cubre tablas `ldrb`, `ldrh` y `ldrsw`.
  - Si una función queda partida igual (una llamada a su mitad), los casos que caen en otro pedazo pasan a
    ser inicios, y el `br` llega a ellos con una llamada de cola.
  - Resultado: ninguna función con FDE del `sdk` ni de `main` queda partida. Antes eran 443 y 788
    funciones, en 6102 y 8361 pedazos de más. En `subsdk0` queda una, partida por un `adrp`+`add` que
    apunta a la mitad de otra función. El descubrimiento termina en 2 rondas (antes 17 en el `sdk` y 7 en
    `main`). Hay 40461 funciones en el `sdk` (antes 46563) y 122961 en `main` (antes 131322).
  - Las estadísticas de `gen.py` y `sw_probe.py` cuentan ahora los `br` de las funciones finales. Antes
    sumaban los de cada ronda. Sitios `br`, antes → ahora:

    | módulo | jump tables resueltas | `br_all_labels` | `br_dispatch` |
    |---|---|---|---|
    | `sdk` | 0 → 555 | 611 → 56 | 9695 → 9695 |
    | `main` | 266 → 1288 | 1063 → 42 | 6343 → 6342 |
    | `subsdk0` | 0 → 333 | 333 → 1 | 4022 → 4021 |

  - `br_dispatch` es un salto por un puntero cargado (vtables, stubs), no una tabla. El que cambia en `main`
    es una tabla `ldrsw` que el corte dejaba en un pedazo sin su `add`.
  - Los 56 + 42 que siguen sin resolver tienen la guarda en un bloque anterior, el índice recargado de otra
    ranura de la pila, o el índice acotado con `and`/`ubfx` sin `cmp`. Usan el respaldo de siempre: cada
    instrucción de la función es una etiqueta y el `br` salta por un `switch`. Como la función ya no está
    partida, igual llegan.
  - Ese respaldo depende de `computed_target`: busca hacia atrás desde el `br` la instrucción que escribe su
    registro y lo da por tabla si es un `add`/`sub` o un `adr`. Comparaba solo los 5 bits bajos de cada
    instrucción, que también son el registro que guarda un store y los destinos de FP/SIMD: en `main`, un
    `stp s9, s8, [sp, #0x80]` entre `add x9, x9, x10, lsl #2` y `br x9` lo cortaba (1.8.2 0x700109c950, 1.9.0
    0x70011b06e0, una tabla `ldrb` con base `adr` y sin guarda). El `br` quedaba como despacho simple y el
    juego moría al llamar al caballo ("no function at 700109c954", issue #22). Ahora `writes_gpr` mira con
    capstone qué registros escribe la instrucción. En la 1.9.0 cambian 2 `br` de `main`, ninguno del `sdk` ni de
    `subsdk0`, y ninguno deja de reconocerse; el otro (0x70003d7a84) ya se resolvía como tabla, así que el C
    solo cambia en esa función. `tools/test_layout.py` tiene su forma (la función K: falla con el `gen.py`
    anterior). En una prueba oculta (1.9.0) no se pudo llegar a esa función: hace falta un caballo que acuda al
    silbido, y ninguno de los saves de prueba tiene uno.
  - Costo: el C con todas las etiquetas crece en `main` de 312k a 485k instrucciones y en el `sdk` de 37k a
    171k. La función más grande sigue siendo la misma (11883 instrucciones, en `main`).
  - `tools/test_layout.py` agrega un módulo sintético con la forma de `printf_core`, y una función partida
    por una llamada a su mitad.

Pendiente:

1. Restos de LA conocidos, sin efecto en la compilación:
   - `ui/launcher.mm` no comprueba la versión: falta el build id del `main` 1.8.2 (`tools/sw_probe.py`).
   - `hle/movie.c` solo deja saltar `movie/opening.webm` (nombre de LA).
   - `hle/framepace.c` y `hle/heaps.c` explican sus valores con casos de LA (siguen siendo genéricos).
   - `CONTRIBUTING.md` es de LASRecomp. (El README y el ícono ya son propios: `docs/icon/icon.svg`, una montaña
     helada con una espada clavada en la cima, dibujo original; `icon_1024.png`, `icon.ico` e `icon.icns` salen de
     él con `rsvg-convert`.)
   - `tools/explore_far.sh`, `explore_input.sh`, `house_input.txt`, `dbgfs.sh`, `fresh_save.sh`: entradas
     de prueba pensadas para la pantalla de título de LA.
   - Pantalla ancha (aspecto desbloqueado) e iconos de botones por control: se fueron con
     `widescreen.c` y `texpatch.c`; para BOTW harían falta ganchos propios.
   - `runtime/nvn/metal.m` y `renderer.m` (solo macOS) se editaron sin compilarlos.
2. Arranque de 1.8.2: con el `sdk` generado entero, x2 corre los constructores del `sdk`.
   - Pasa por `printf_core`. Arreglado en `gen.py`: ver "Jump tables" arriba. Medido: entra 4 veces (dos
     `vfprintf` con `"%s"` y `"%02d - %s"`), y su tabla `ldrb` salta a 0x700292070c y a 0x700292063c.
   - Resuelto (2026-10-14), en orden de aparición:
     - TIPC: el `sdk` de 1.8.2 le habla a `sm:` por TIPC (tipo de mensaje 16 + comando; 15 cierra la sesión).
       `runtime/ipc.c` lo atiende con el mismo `sm_handler`: los argumentos son las palabras crudas, sin
       `SFCI`, y la respuesta lleva el resultado como primera palabra, sin `SFCO` ni alineación.
     - `romfs.bin`: el runtime lee la RomFS desde esa imagen, que arma la pantalla de setup. Para
       desarrollo sin launcher: `build/savagewind.exe extracted --build-romfs` (4,5 min, 15,1 GB).
       Desde #99 ya no hay imagen en disco: `runtime/romfs_image.c` la sirve desde `extracted/romfs/` (cabecera
       y tablas en memoria, los datos de cada fichero leídos del suyo) y la pantalla de inicio borra el
       `romfs.bin` de versiones anteriores. `--dump-romfs <fichero>` escribe la imagen para compararla.
     - `IApplicationFunctions` 210 (`GetHealthWarningDisappearedSystemEvent`): el juego espera ese evento
       antes del primer frame; el stub no devolvía handle y el juego sondeaba el handle 0 para siempre.
       Ahora devuelve un evento ya señalado.
     - Versión de NVN: `main` 0x7100d802e0 solo crea su sistema gráfico si el dispositivo informa API 55 con
       minor ≥ 15; con el 53.207 de LA seguía sin él y después leía un objeto nulo. Además pide las
       propiedades 82, 85 y 92 (tamaños por defecto de memoria de comandos, compute y control de la cola).
     - `NVNsamplerBuilder` mide 64 bytes en NVN 55. El HLE guardaba 80 (device + `NvnSamplerDesc`) y pisaba
       el frame pointer guardado del llamador (`main` 0x71000156f0 lo arma en la pila). El estado del
       builder ahora está empaquetado en 48 bytes.
   - Resuelto (2026-10-15 y 16), en orden de aparición:
     - Lista rota del heap de efectos (`main` 0x7100d8df90 giraba para siempre): la escribía
       `hle_nvnTextureViewSetDefaults`. `NVNtextureView` mide 16 bytes en NVN 55 y el HLE guardaba 0x28; el
       juego pone las vistas al final de bloques de un `sead::ExpHeap` y pisaba la cabecera del bloque
       siguiente. `NvnTextureView` ahora está empaquetado en 16 bytes. Lo encontró la trampa de escrituras
       por página (`SAVAGEWIND_WATCH*`, abajo).
     - Tamaños reales de NVN 55, leídos de los `SetDefaults`/`Initialize` del driver del `sdk` (sus funciones
       se ubican con `SAVAGEWIND_NVN_REAL=1`): `TextureView` 16, `SamplerBuilder` ≥ 48, `QueueBuilder` 60,
       `WindowBuilder` 48, `BufferBuilder` 40, `DeviceBuilder` 24, `EventBuilder` 16, `MemoryPoolBuilder` 64,
       `TextureBuilder` ≥ 104; objetos: `Sampler` ≥ 64, pools 32, `Event` 40, `Buffer` 48,
       `CommandBuffer` 160, `MemoryPool` 256, `Program` 128, `Sync` ≥ 56.
     - `nvnDeviceGetInteger`: la tabla de LA se reemplazó por la del driver 55.15 (`sdk` 0x71004c2c00 →
       0x71004c6310 → tabla 0x71004b6070); los valores que dependen del dispositivo son los de un Tegra X1.
     - Audio: con revisión ≥ 9 el SDK lee la versión 2 del estado de efectos (0x90 bytes cada uno) y la
       sección de rendimiento de 0x10; con otro tamaño abortaba la actualización (0x5299).
     - `nfp:user` (amiibo): `IUserManager` → `IUser` sin lectores; antes el juego leía un objeto nulo.
     - `svcGetInfo` 18 (`ProgramId`) sale de `main.npdm` y 20 (`UserExceptionContextAddress`) devuelve una
       zona de TLS: `SetDyingMessageRegion` la necesita no nula. El id base de `aoc:u` sale del program id.
     - Memoria de comandos: el juego graba cada display list una vez para medirla ("calc resource") y pide
       exactamente esa memoria al heap del modelo, que la `ResourceSizeTable` dimensiona para NVN real. Con
       los records de Savage Wind (≥ 80 bytes por comando) los heaps se quedaban cortos, la asignación fallaba
       y la lista llegaba con pool nulo. Ahora el juego ve un costo estimado por comando (≤ lo que escribe NVN)
       y los records van a memoria del host, en "sombras" por rango de memoria guest (`runtime/nvn/cmdbuf.c`).
     - Hilos: BOTW llama a NVN desde varios hilos. El renderer (submit, flush, present, texturas finalizadas)
       queda detrás de un lock recursivo (`gpu_lock`).
     - Texturas: los comandos guardan el `NVNtexture` del guest y lo resuelven al ejecutarse; el juego
       finaliza y vuelve a inicializar render targets en el mismo objeto, copia objetos `NVNtexture` y
       finaliza una copia mientras usa otra. `nvnTextureFinalize` solo suelta la imagen Vulkan; el objeto del
       host vive hasta un reinicio.
     - Buffers de onda fuera de la memoria guest se saltean en vez de leerse; pools de descriptores: hasta 64
       por envío.
   - Resuelto (2026-10-16, segunda tanda):
     - Shaders: `TLD4`/`TLD4S` (gather), `TMML` (LOD), formas bindless con el handle cargado de `c[2]` justo
       antes, y `LDS`/`STS` (memoria compartida de compute, tamaño en la sección de control +0x754). Todos los
       shaders del arranque traducen.
     - `nvnMemoryPoolMapVirtual`: el juego mapea su pool virtual de texturas (0x7300000) entero sobre uno físico;
       el pool virtual toma esa memoria.
     - Render targets re-creados cada frame: BOTW finaliza e inicializa otra vez sus targets en la misma memoria
       y muestrea los objetos viejos (entradas del texture pool, copias de `NVNtexture`). La imagen de una textura
       finalizada se estaciona y la toma la próxima textura con la misma memoria y forma; un objeto finalizado se
       resuelve a la textura hecha después en su memoria. Era la imagen "en bloques".
     - Cuelgue intermitente (la mitad de las corridas, entre los frames 280 y 1500): `svcWaitProcessWideKeyAtomic`
       soltaba el mutex antes de marcar el condvar, y quien señalaba se salteaba `svcSignalProcessWideKey`.
     - Entrada: BOTW lee los controles con `nn::hid::GetNpadStates` (plural), que el HLE no atendía.
     - Imágenes de texturas finalizadas que se seguían muestreando no se liberaban (el menú agotaba la memoria de
       video en 2 minutos).
     - Escala de render targets (la "resolución interna" de LA): apagada salvo `SAVAGEWIND_SCALE_TARGETS=1`; con
       ella el pipeline diferido de BOTW mostraba solo lo emisivo. Activa desde el 2026-10-19 (ver "Resolución
       interna y escalador").
   - Resuelto (2026-10-16, tercera tanda): la iluminación diferida del santuario. En orden de impacto:
     - Listas pregrabadas que se perdían. El juego da a la lista de material de cada composición de luz una
       ventana de 0x200 bytes de memoria de comandos, graba los ~0x18 bytes que midió antes y empaqueta la
       siguiente justo detrás. El HLE daba por muerta toda lista que se solapara con la ventana nueva aunque nada
       se escribiera encima, y el frame la seguía llamando: ejecutaba otros records. Así se perdía el uniform
       buffer con el ID de material (`c4` del VS, 432 bytes, el ID en +0x1a8), que las composiciones comparan
       con test EQUAL contra la D16 que escribe el G-buffer: no se componía ninguna luz. Ahora una lista muere
       cuando la grabación escribe de verdad sobre su memoria (`cmdbuf.c`, `shadow_claim`).
     - `nvnCommandBufferBindTexturesDeferred` (el stub no ataba nada): el juego ata así las texturas de la
       composición (normales, albedo, profundidad lineal, probe, buffers de luz). Lee los handles al ejecutar.
     - Las texturas de los comandos se resuelven al grabar (`nvn_texture_host`), como en el hardware. La cadena
       de exposición (192x192 → … → 3x3) reinicializa el mismo `NVNtexture` en cada paso dentro de un command
       buffer: resueltas al ejecutar, todos los pasos dibujaban en el último.
     - Una imagen Vulkan por memoria y forma (`textures.c`): los objetos `NVNtexture` sobre la misma memoria con
       la misma forma comparten imagen (entradas del pool con objetos finalizados, copias de objetos, targets
       recreados). Reemplaza el estacionamiento de imágenes y `nvn_texture_current`.
     - Cubemaps muestreados como cube array (probes de 64x64 R11G11B10): salían negros.
   - Resuelto (2026-10-16, audio):
     - El juego mezcla con el AudioRenderer de su propio `sdk` (servidor y DSP recompilados) en vez del HLE de
       `audren:u`, que con el SDK de 1.8.2 recibía voces con formato 0 y buffers sin sentido y quedaba mudo
       (sección "Audio").
     - Kernel: un `svcWaitProcessWideKeyAtomic` que vencía volvía con el mutex tomado, y el SDK lo vuelve a tomar
       él mismo (`InternalConditionVariableImplByHorizon::TimedWait`, `sdk` 0x71001492bc, como en la consola): el
       hilo esperaba su propio mutex y colgaba a los demás (el hilo de audio del juego, a los 30 s). Ahora vuelve
       sin el mutex. Tampoco cuenta como vencido un timeout que llega cuando otro hilo ya lo despertó o le dio el
       mutex.
     - Kernel: `svcArbitrateUnlock` y `svcSignalProcessWideKey` ceden el núcleo después de soltar, no antes: el
       hilo que cedía primero dejaba esperando a los demás hasta recuperar su núcleo.
   - Resuelto (2026-10-16, santuario):
     - El pasillo marrón que lleva al pedestal se veía estirado a lo largo en casi todas las corridas: su textura
       (1024x2048, UV que se repite) se muestreaba con un sampler de clamp. El juego copia objetos `NVNsampler`
       por valor y registra la copia en el pool después de finalizar el original; cada objeto apuntaba a una
       descripción propia que `nvnSamplerFinalize` liberaba, y la copia leía la memoria ya reusada por otro sampler.
       Ahora las descripciones se comparten por configuración y no se liberan (`objects.c`).
     - El shader de partículas que fallaba con "bindless tmml (handle not from c[2])" traduce (una burbuja pequeña
       del santuario); no tenía que ver con lo anterior.
   - Ahora: el menú principal se ve bien. Con `Continue` (`SAVAGEWIND_INPUT="900:A:8,1400:A:8,1900:A:8"`) el
     Santuario de la Resurrección se ve iluminado: Link, el pedestal, la puerta, los pilares y el título del
     lugar. Lo conocido:
     - Niebla azul muy fuerte; en algunas corridas, un rectángulo arriba a la izquierda (~1/4,5 de la
       pantalla) con otro tono y una franja de bruma blanca. Vienen de los efectos a media resolución que se
       componen sobre el HDR (pases de niebla/volumen después de la luz).
     - 6-8 fps en el santuario (~3.400 draws por frame). Con `SAVAGEWIND_VK_PROF=1` el frame es ~20 ms de
       backend, ~25 de presentación y ~75 del lado del juego; no cambia con el arreglo de listas.
     - En algunos frames, una textura 3D de 32x32x32 (BC4) se muestrea como 2D y devuelve negro
       (`[tex] type mismatch`).
     - El resto de los `nvnCommandBufferBind*Deferred` (imágenes, uniform y storage buffers, texturas y
       samplers separados) sigue sin implementar: el juego los busca pero no los llamó en estas corridas.
       `renderer.m` (Metal) no ejecuta `CMD_BIND_TEXTURES_DEFERRED`.
     - Sonido: suenan el jingle de `Continue` (`Sys_GameStart.bfstm`) y el ambiente del Santuario. En el título y
       el menú el juego no pide ninguna voz (ni un `AcquireVoiceSlot` en 50 s, y no lee nada de
       `Sound/Resource`), así que ahí no suena nada.
   - Teclado y ratón (2026-10-16): defaults pensados para BOTW (tabla en el README), cualquier tecla o botón del
     ratón remapeable (`BIND_MOUSE` + botón en `g_bind_key`), acción "Walk (hold)" (C: el stick izquierdo a 40%) y
     el ratón como cámara: con la ventana enfocada, sin menú y sin script, el cursor queda retenido (modo relativo de
     SDL) y su velocidad se vuelve el stick derecho (≈1200 px/s a fondo con sensibilidad 1, más un 12% para pasar la
     zona muerta del juego; `SAVAGEWIND_MOUSE_SCALE`, `SAVAGEWIND_MOUSE_DEADZONE`). Esc suelta el cursor y un clic
     en la ventana lo vuelve a retener. Ajustes: `mouse_camera`, `mouse_sensitivity`, `mouse_vertical_sensitivity`
     (el eje vertical, en fracción de la sensibilidad: 0,5 a 2) y `mouse_invert_y`.
     en la ventana lo vuelve a retener. Ajustes: `mouse_camera`, `mouse_sensitivity`, `mouse_invert_y`.
   - Ratón como cámara, nativo (2026-10-25, `runtime/hle/input.c`, `tools/patches.py` `_cam_*`): el ratón ya no
     pasa por el stick. Las cámaras son acciones de IA del actor `GameRomCamera` (`CameraChase`, `CameraAiming`,
     `CameraHorse`...; sus fábricas se registran por el CRC32 del nombre) y todas leen el stick derecho con una
     sola función (1.9.0 `0x7000d57fe0`): lee `+0x128` del mando, aplica una zona muerta, eleva la inclinación al
     cuadrado (`v |v|`) y la deja en el círculo unidad. Giran eso × sus velocidades (`latStickScale` /
     `lngStickScale` del AIProgram; la velocidad de cámara del juego, el flag `StickSensitivity`: 0.8..2.4, 1.6 por
     defecto; ×1.78 en horizontal) × la duración del frame (VFR). Las de tipo chase (`CameraChase`,
     `CameraShieldSurfing`, `CameraWakeboard`) elevan además la parte vertical al cuadrado (`y |y|`). Con el ratón
     como stick eso daba una cámara que apenas se movía con movimientos lentos, se aceleraba con los rápidos y tenía
     velocidad máxima. Ahora, mientras el ratón tiene la cámara, el runtime responde esa función (`rt_cam_stick`):
     el movimiento del ratón en el frame (tomado en `rt_vfr_update`) en grados, 0.022 por cuenta × la sensibilidad
     (como Quake y Source), entre el giro de una inclinación completa en ese frame; ni recortado ni curvado. La parte
     vertical lleva ×1.78, en las chase se salta el cuadrado (`rt_cam_pitch`) y en `CameraAiming` / `CameraAiming2`
     usa la escala de la horizontal (`rt_cam_pitch_scale`), así que los dos ejes giran igual. Medido con
     `SAVAGEWIND_HOST_INPUT` ("frame:mouse:dx/dy:frames") y `SAVAGEWIND_MOUSE_LOG`: 600 cuentas al 100% giran
     13.20° en yaw y en pitch, a 30 y a 60 fps; 400 cuentas en un frame, 8.80° (un stick a fondo da 2.85° por frame
     a 30 fps); con el arco, 0.308° por cada 20 cuentas en los dos ejes (su `lngStickScale` 0.7). Lo que la imagen
     se sigue moviendo al soltar con el arco es el rig de la cámara siguiendo la pose de Link: los ángulos de
     `CameraAiming` se paran en el mismo frame (`SAVAGEWIND_MOUSE_LOG_X19`), y con el stick pasa igual. Las opciones
     Camera X-axis / Y-axis (Inverted) del juego las aplica el mando de las cámaras (el id 3 del gestor de mandos,
     un envoltorio que guarda `objeto + 8` en la lista): cada frame lee `CameraRLReverse` / `CameraUpDownReverse` y le
     da la vuelta a ese eje de su stick derecho. El ratón no pasa por él, así que `_cam_reverse` le pasa los flags al
     runtime (`rt_cam_reverse`) y este le da la vuelta igual a su movimiento: con las dos en Inverted (en una copia
     de `option.sav`) el ratón gira +4.40° / −4.40° en vez de −4.40° / +4.40°, como hacía al pasar por el stick.
     Los sitios son opcionales (`OPTIONAL_GROUPS` en `tools/game_sites.py`): si en una versión un patrón no encuentra
     su sitio, el grupo entero queda fuera y el ratón vuelve a mover el stick (sin el segundo grupo, solo pierde las
     opciones de inversión del juego). `SAVAGEWIND_MOUSE_NATIVE=0` también vuelve al stick.
   - Controles de movimiento (2026-10-22, `runtime/hle/motion.c`): BOTW importa cuatro funciones del sensor de seis
     ejes de nn::hid: `GetSixAxisSensorHandles` y `StartSixAxisSensor` (cuando cambia el estilo del mando,
     `FUN_7100d9e24c`), `GetSixAxisSensorStates` (cada frame, las 16 muestras más nuevas de cada handle) y
     `SetGyroscopeZeroDriftMode`; la familia `SevenSixAxisSensor` solo se usa en modo VR (`nn::pl::IsVrMode`). El
     addon de seis ejes (`FUN_7100feb48c`, vtable 0x7101bbe588) copia aceleración, velocidad angular, ángulo y
     dirección de las 16 muestras a los ejes de sead (x = -x, y = z, z = y), cuenta las nuevas por el número de
     muestreo (máximo 16) y toma una aceleración cero o una dirección identidad como "sin datos". El apuntado por
     movimiento (`FUN_7100c28eac`, opción del juego GyroOnOff) lee la velocidad angular y la dirección de la muestra
     más nueva y compara la dirección con la del frame anterior. Antes no había implementación: el handle no existía
     y el juego veía el mando quieto, así que los santuarios de inclinación no se podían resolver.
     Ahora la orientación sale del giroscopio y el acelerómetro del mando (eventos de sensor de SDL3, pasados a los
     ejes de Switch: X derecha, Y adelante, Z arriba; aceleración en G como dirección de la gravedad, giro en vueltas
     por segundo), fusionados en el hilo principal: el giroscopio la gira, la gravedad corrige la inclinación poco a
     poco (Mahony, solo con |a| entre 0,75 y 1,25 G) y el sesgo del giroscopio se aprende en reposo. Sin sensores, la
     tecla de inclinar (T por defecto, `bind_key_tilt`) mantenida hace que el ratón incline el mando (0,2° por
     píxel, hasta 50°; al soltarla vuelve a nivel en ~0,15 s) en lugar de girar la cámara. Lo que recibe el juego es
     esa orientación como guiñada / cabeceo / alabeo con la sensibilidad y la inversión del jugador aplicadas a
     guiñada y cabeceo; las muestras se generan a 200 Hz según el tiempo pasado, y la velocidad angular de cada una
     es el giro desde la anterior, así que todo concuerda con la dirección. Ajustes: `motion_sensitivity`,
     `motion_invert` (`off`, `up_down`, `left_right`, `both`); la opción del juego "Controles por movimiento" pasa a
     la página Controles. Pruebas: `SAVAGEWIND_INPUT` acepta `TILT_UP/DOWN/LEFT/RIGHT` y
     `TURN_UP/DOWN/LEFT/RIGHT`; `SAVAGEWIND_MOTION_LOG=1` registra la orientación una vez por segundo.
   - Iconos de botones (2026-10-22, `runtime/nvn/glyphs.c`): BOTW dibuja los botones de sus avisos (HUD, menús,
     diálogos) como caracteres de su propia fuente de mapa de bits `External_00.bffnt` (en `Font/Font_*.sbfarc`,
     igual en todas las regiones): 51 glifos en U+E040..E091, celdas de 50x50 (a 51 px) en una hoja BC4 de 256x1024
     guardada de abajo arriba (A B X Y en E040..E043, L R ZL ZR en E044..E047, la cruceta en E049 y E060..E065, Home
     en E04A, + y - en E04B/E04C, direcciones y pulsaciones de los sticks en E050..E05B, los sticks en E05D..E05F).
     Los textos no llevan esos caracteres: una etiqueta del mensaje los elige en tiempo de ejecución, así que se
     cambia la hoja, no los textos. nn::font copia la hoja (los bytes del archivo desde 0x2000) a un pool de memoria
     propio: la textura se reconoce por tamaño, formato y un hash FNV-1a de sus 0x20000 bytes, y sus téxeles se
     sustituyen al subirla (ya desentrelazados) por glifos dibujados aquí en las mismas celdas, dentro de la caja del
     glifo original (los anchos del juego no cambian). Teclado: teclas con la tecla asignada ("E", "Shift", "Space",
     flechas) y ratón con el botón asignado; el stick izquierdo como "WASD" y el derecho como ratón con flechas (o sus teclas si el
     ratón no mueve la cámara). Xbox y PlayStation: sus botones a través de las asignaciones del mando (A B X Y o
     cruz / círculo / cuadrado / triángulo, LB/RB/LT/RT o L1/R1/L2/R2, Menu/View u Options/Create, cruceta). Los
     sticks y el resto de símbolos quedan los del juego. Nintendo: los glifos del juego, movidos de celda si las
     asignaciones del mando no son las de fábrica. Formas trazadas con SDF; rótulos en Fira Sans (OFL, ya incluida).
     Ajuste `button_icons` (Controles > Iconos de botones: Automático, Teclado, Xbox, PlayStation, Nintendo);
     Automático sigue al último dispositivo usado (tecla, botón o giro del ratón, o botón / stick del mando) y la
     hoja se vuelve a subir al cambiar (`glyphs_generation`). `SAVAGEWIND_GLYPH_DUMP=1` guarda cada hoja subida en
     `build/glyph_sheet_<n>.pgm`.
     Pendiente (2026-10-22, visto por el usuario jugando con teclado y ratón): el aviso de acción del HUD (el rombo
     de cuatro botones con "A Take" al recoger un objeto) sigue mostrando los botones de Switch. Ese rombo no sale de
     `External_00.bffnt`: lo más probable es que sea una imagen del layout del HUD (bflim), que `glyphs.c` no cubre.
   - Ritmo de frames (2026-10-16): `nvnQueuePresentTexture` espera el turno de cada frame según el intervalo de la
     ventana (vsyncs por frame) y nunca presenta más rápido que el tope (`SAVAGEWIND_FPS_CAP`, 60 por defecto; 0 sin
     tope). BOTW pide intervalo 2 (30 FPS, como en la consola; 3 en algunas cargas): el menú, que iba a 130 FPS con
     sus animaciones aceleradas, va a 30. Desde 2026-10-20 el ritmo es la frecuencia elegida (ver "Frecuencia de
     frames" más abajo).
   - Microtirones en el mundo abierto (2026-10-19): el bucle de frames de BOTW (sead `GameFrameworkNx`, main
     0x700101287c) cronometra cada frame y pasa a intervalo 3 (20 FPS) tras tres frames seguidos de más de 1,9 vsyncs
     (31,7 ms); vuelve a 2 unos frames después. Cada tirón costaba así ~8 frames de 50 ms. Lo que alargaba los frames:
     - El trabajo del backend (traducir la lista, presentar, esperar a la GPU) corría con el núcleo emulado del hilo
       tomado y los demás hilos de ese núcleo esperaban. Ahora lo suelta (`kernel_host_work_begin/end`;
       `SAVAGEWIND_HOST_WORK_CORE=1` vuelve a retenerlo).
     - BOTW graba el frame entero en una sola lista: la GPU no empezaba hasta que la lista estaba traducida (~10-15 ms)
       y el juego espera a la GPU del frame anterior (`nvnSyncWait`). Ahora va a la GPU por partes, en el límite de un
       pase cada ~300 draws (`SAVAGEWIND_VK_SUBMIT_DRAWS=<n>`, 0 = una por lista), salvo después de un shader que
       escribe en la memoria del juego (memoria global): ejecutado mientras se traduce el resto, la CPU copiaba datos a
       medio escribir y la GPU fallaba con una escritura inválida (device lost en ~1 de cada 7 cargas del mundo).
     - `vkQueuePresentKHR` (NVIDIA, Windows, en ventana) espera a que la GPU termine el frame: 13-18 ms en uno de cada
       cinco. Ahora presenta un hilo propio con una segunda cola de la misma familia (`SAVAGEWIND_VK_PRESENT_THREAD=0`
       presenta en el hilo que llama).
     - Shaders nuevos (compilación síncrona): la traducción Maxwell → GLSL costaba 1-4 ms por programa en el mundo.
       Ahora se guarda en memoria por etapa, binario y clave (los programas comparten shaders) y en disco
       (`cache/xlate/`, se invalida al recompilar `translate.c`); `SAVAGEWIND_NO_XLATE_CACHE=1` la desactiva y
       `SAVAGEWIND_XLATE_VERIFY=1` vuelve a traducir y compara (0 diferencias en un paseo completo). El módulo
       SPIR-V y sus bindings también se hacen una vez por shader traducido, no por programa.
     - Coste por draw al traducir (~5.000 draws por frame en el mundo): cada draw volvía a fijar el pipeline, unas 20
       piezas de estado dinámico, los descriptor sets y los vertex buffers aunque fueran los del draw anterior. Ahora
       graba solo lo que cambia (`SAVAGEWIND_VK_NO_DYN_CACHE=1` lo graba todo); los descriptor sets de contenido
       idéntico dentro de una entrega son uno solo (~45 % compartidos; `SAVAGEWIND_VK_NO_SET_CACHE=1` los rehace); el
       programa y el pipeline del draw anterior se reutilizan sin rehacer su hash, y las listas de texturas por liberar
       se recorren una vez por frame, no con cada textura nueva. Traducción en la zona más cargada del paseo (frames
       1645-1900, ~4.700 draws, intervalo fijo): 15,0 → 11,4 ms por frame (3,2 → 2,4 µs por draw); frames de más de
       40 ms en todo el paseo: 79 y 49 → 9 y 8 (dos corridas de cada uno; los que quedan son capturas, compilaciones
       al cargar el mundo y picos del juego). `SAVAGEWIND_VK_PROF=2` deja solo los tiempos por lista (los de cada draw
       cuestan ~0,3 µs por draw); la línea `[hitch]` termina con `exec waits` (esperas a la GPU dentro de la
       traducción).
     - Render targets que el mismo draw muestrea y escribe (~240 draws por frame en el mundo: el terreno, fs
       `50fc37edcd79cf02`, hace un OR de bits en el canal G del target 0 del G-buffer leyendo su propio píxel; también
       `60f90627d72c7d08` y `fd051fb152ee8e40`, sin barreras del juego entre ellos): antes se copiaba el target de
       1600x900 antes de cada draw, con barrera completa y pase nuevo (6,5 de 17 ms de GPU en un frame cargado). Ahora
       el target es un feedback loop (`VK_EXT_attachment_feedback_loop_layout` en layout GENERAL con
       `VK_KHR_unified_image_layouts`) y entre esos draws va una barrera dentro del pase
       (`VK_KHR_dynamic_rendering_local_read`): cada draw ve lo que escribieron los anteriores en su píxel, como con
       las copias. Frame 1750 del paseo (`SAVAGEWIND_GPUPROF_AT=1750`, cuatro corridas de cada uno): 16,0-16,2 →
       9,0-10,9 ms de GPU (copias y sus pases: 8,5 → 0,4 ms); GPU por cada 2 s: ~1.055 → ~615 ms. Con menos carga el
       driver baja el reloj de la GPU (~1.950 → ~900 MHz), así que los ms de después están medidos a menos de la mitad
       de reloj. Sin errores de la capa de validación. `SAVAGEWIND_VK_NO_FEEDBACK_LOOP=1` vuelve a las copias;
       `SAVAGEWIND_FEEDBACK_DUMP=<frame>` vuelca en crudo los targets afectados de ese frame para comparar.
     - Caché de pipelines y precalentamiento (2026-10-19): con las cachés calientes, cada programa nuevo aún costaba
       en el hilo de render leer su traducción y su SPIR-V, crear módulos y layout, y pedir el pipeline al driver: al
       cargar el mundo eran ~900 compilaciones en 2 s (70-80 ms en un frame), y 1,7 s la primera vez que se leían los
       ficheros de caché recién escritos. Ahora:
       - `cache/vk_pipelines.bin` lleva una cabecera con la GPU (vendor, device), el driver (versión y UUID de la
         caché), la build del traductor de shaders y una suma del contenido: si algo no coincide, o el fichero está
         dañado o cortado, la caché empieza vacía y se reescribe. El formato anterior (los datos del driver solos) se
         acepta una vez si la cabecera del driver coincide. Se guarda al salir y, mientras se juega, desde un hilo
         propio unos segundos después de que dejen de salir pipelines nuevos (como mucho cada 30 s; como poco cada
         2 min si hay nuevos), en un fichero temporal que sustituye al anterior de una vez: un cierre forzado pierde
         poco. Qué pipelines son nuevos lo dice el feedback de creación del driver (acierto de caché o no).
       - `cache/vk_pipelines.list`: cada pipeline que se construye queda apuntado (las traducciones de sus etapas, por
         su nombre en `cache/xlate/`, y el estado fijo de su clave), sin repetidos y con una suma por registro: los
         dañados se descartan al leer y el fichero se reescribe. Misma cabecera salvo el driver: un driver nuevo vacía
         la caché de pipelines, y la lista es lo que la rehace antes de que el juego la necesite.
       - Al arrancar, hilos de trabajo construyen todo lo de la lista mientras el juego arranca (los ~870 pipelines
         del paseo: ~0,1 s con `vk_pipelines.bin`; 1.500 sin ella, 0,6 s) en las tablas por contenido donde
         buscan los draws: programas por par de shaders traducidos, pipelines por shaders y estado fijo. El primer
         frame con pinta de juego (800 draws o más) espera a que terminen y, si tarda más de 0,3 s, la ventana muestra
         una barra de progreso. Un draw que necesita un pipeline que un hilo está construyendo lo espera; uno al que
         aún no han llegado lo construye él, como antes. Lo que no está en la lista se compila al primer uso, síncrono
         como antes (en asíncrono se perdían draws de una sola vez), y se añade.
       - La línea `[vk] t=` dice `compile N (warm M)`: N programas y pipelines construidos donde hicieron falta, M
         encontrados ya hechos. `SAVAGEWIND_CACHE_DIR=<carpeta>` cambia la carpeta de las cachés (pruebas);
         `SAVAGEWIND_VK_NO_PIPELINE_CACHE=1` ni carga ni guarda la caché de pipelines; `SAVAGEWIND_NO_SHADER_WARM=1`
         quita el precalentamiento (la lista se sigue apuntando); `SAVAGEWIND_PSO_SLOW_MS=<n>` alarga n ms cada
         pipeline precalentado para ver la espera (guarda la barra en `build/pso_wait.png`).
       - La clave de los pipelines copiaba los ocho formatos de color del pase aunque solo cuenten los `ncolor`
         primeros, y los ocho bytes del blend: los demás eran restos de pases anteriores y hacían pipelines
         duplicados (el paseo usa ~870 distintos; se construían ~1.500). Ahora la clave lleva solo lo que el pipeline
         usa.
       Paseo con guion (intervalo fijo, del primer frame del mundo al 3000), cachés calientes, cuatro corridas antes y
       tres después (dos con la caché del propio paseo y una con una copia de la caché de la carpeta principal):
       compilación en el hilo de render 5.898 / 865 / 362 / 449 ms → 16 / 1 / 32 ms; compilaciones ~1.700-1.850 →
       31 / 0 / 19 (en la carga del mundo, ~900 en 2 s → 0-7); frames con más de 5 ms de compilación 88 / 26 / 15 /
       15 → 0 / 0 / 2; frames de más de 40 ms 56 / 51 / 9 / 11 → 24 / 4 / 5 (los 24, sin compilaciones: el ritmo de
       frames durmió de más un rato, como en otras corridas); máximo 1.732 / 229 / 176 / 125 → 89 / 149 / 91 ms. El
       5.898 es la primera corrida tras llenar las cachés: leer por primera vez los ficheros recién escritos es lento.
       Primer frame del mundo a los 20,5-22 s antes y después. En frío (sin cachés) nada cambia salvo los duplicados:
       la lista se llena en esa primera sesión. Lo que queda: contenido nunca visto (en una corrida apareció algo
       nuevo hacia el frame 3050: ~100 pipelines que el driver tampoco tenía, 50 ms cada uno, 8 frames de más de
       150 ms) y, en la carga del mundo, 80-175 ms del propio juego en sus primeros frames.
     - Precompilación de todos los shaders (2026-10-22): lo que el precalentamiento no cubre es lo nunca visto. La
       primera vez que el juego dibuja con un shader hay que traducirlo, pasarlo a SPIR-V y que el driver lo compile
       (40-100 ms por pipeline); en el paseo con la caché del driver vacía eran 25 s de compilaciones en el hilo de
       render. Ahora, si se quiere, se precompila todo de una vez:
       - Dónde: al instalar, la casilla "Precompilar todos los shaders" (activada por defecto; se hace tras preparar
         los ficheros del juego) y, cuando se quiera, Savage Wind Configuration > archivos del juego > Shaders >
         "Precompilar shaders" (dice "Hecho", "Volver a hacer" o "Sin hacer", con el número de shaders y la fecha de
         la última vez). Una barra con el paso (leer los archivos, traducir y compilar, pipelines) y la cuenta; Esc o
         B la para y lo hecho se queda. `savagewind --precompile-shaders` hace lo mismo sin el menú (ventana oculta
         con `SAVAGEWIND_HIDDEN=1`); `savagewind --scan-shaders` solo cuenta lo que encuentra y cuántos de los shaders
         del registro de claves (abajo) están entre ellos (`SAVAGEWIND_SCAN_LIST=1`: shaders por archivo).
       - De dónde salen (`runtime/nvn/shader_archive.c`): de la RomFS del propio usuario (`romfs/`), en los
         archivos desde los que el juego sube sus shaders a NVN: FSHA con ficheros BNSH de nn::gfx (materiales,
         terreno, hierba: `*.sbfsha`, `*.bfsha` dentro de `*.ssarc`), SHARCB de agl/gsys (`*.sharcb`: postproceso,
         sombras, luces), los efectos VFXB (`*.sesetlist`, también dentro de `*.pack` y `*.sbeventpack`) y el de las
         capas (`Layout/Common.sblarc: bgsh/__ArchiveShader.bnsh`), abriendo Yaz0 y SARC por el camino; y dos BNSH
         en los datos del ejecutable (`exefs/main`). 13.997 shaders distintos (6.569 de vértices, 7.356 de
         fragmentos, 72 de cómputo) en 1.005 archivos; leer los ~2 GB candidatos tarda menos de 2 s. Todos los
         shaders que dibuja el paseo están entre ellos (306 de vértices, 352 de fragmentos, 9 de cómputo; sin el
         ejecutable faltaban 7 y 13). Los formatos (registro de SHARCB, sección de control con magic `0x98761234`:
         +0x700 cbuf, +0x704 bytes de código, +0x748 grupo local, +0x754 memoria compartida) están en los
         comentarios del fichero. 24 no se traducen (el registro dice cuáles y por qué): 20 de cómputo de
         `agl_technique_pfx.sharcb` con instrucciones de superficie que el traductor no conoce y 4 de fragmentos de
         `agl_common.sharcb` (`tlds info 6`); el paseo no usa ninguno.
       - Qué variante: una traducción depende del binario y de una clave de estado (`ShaderKey`). La clave ahora se
         reduce a lo que el traductor lee de verdad (`vk_key_normalize`, con las máscaras del SPH): el tipo de cada
         atributo solo para los que el shader de vértices lee, las salidas de vértices solo las que lee el de
         fragmentos, el tipo de cada target solo para los que escribe, y en los de fragmentos nada del estado de
         vértices; un mismo shader ya no se traduce dos veces por restos de estado. Cada clave nueva queda en
         `cache/vk_shader_keys.list`, y la precompilación saca de ahí cómo dibuja el juego (en BOTW: `flip_y` 0,
         `depth_minus_one` 1, targets float, atributos float salvo el 6 de `uking_mat` (uint) y el 2 de
         `uking_grass` (uint → float), que también van en una tabla fija para la primera vez sin registro). Cada
         shader se traduce con la clave predicha y con las del registro que sean suyas; las predichas coincidieron
         con todas las registradas.
       - Traducciones y SPIR-V: en todos los hilos (la traducción ya es reentrante), a `cache/xlate/` y
         `cache/spirv/` comprimidos con zlib (SPIR-V 1,8 GB → 448 MB; traducciones 137 MB), en carpetas con el id de
         quien los hizo: `TRANSLATOR_ID` (hash de `translate.c` y `shader.h`) y `SPIRV_ID` (hash de `spirv.c`, el
         paso GLSL → SPIR-V, que sale de `shaders.c`), los dos del Makefile. Un traductor nuevo usa carpetas nuevas y
         un hilo borra las de otros ids (`SAVAGEWIND_CACHE_KEEP=1` las deja); las cabeceras de las cachés de
         pipelines llevan los dos ids. Una build sin el Makefile toma su fecha de compilación como id.
       - Pipelines: con `VK_EXT_graphics_pipeline_library` y enlace rápido (sí en la RTX 3060) cada shader de
         vértices y de fragmentos se compila una vez en una librería propia, sin el estado fijo, y un pipeline se
         enlaza de esas dos y de dos librerías solo de estado (entrada de vértices; salida: targets, blending,
         máscaras). Otro blending, otros formatos de target u otra disposición de vértices cuestan un enlace y no
         media compilación (con los mismos shaders y pipelines enteros: otro blend 5,4 ms, otro formato 3,5, otra
         máscara 4,6, el primero ~30 ms), y la precompilación compila todos los shaders sin saber con qué estado se
         dibujarán. Los layouts van con sets independientes, uno por etapa. Los pipelines con feedback loop se hacen
         enteros, como antes, y también cuando un enlace falla. `SAVAGEWIND_VK_NO_GPL=1`: pipelines enteros para
         todo. Enlazados así no cuestan más GPU: paseo con cachés calientes, dos corridas de cada, 701-709 ms de GPU
         por cada 2 s con librerías y 702-703 con pipelines enteros (traducción en CPU 561-571 y 577-578 ms), los
         mismos tirones y sin diferencias a la vista en las capturas. Sin esa extensión (otros drivers) se
         precompilan traducciones y SPIR-V; con `SAVAGEWIND_PRECOMPILE_PIPELINES=1`, también un pipeline entero por
         programa con el estado más común (~0,85 GB de caché y poca ayuda: 12,7 → 8,4 s en el paseo, porque otro
         estado vuelve a costar casi una compilación).
       - Las librerías precompiladas van a `cache/vk_precompiled.bin` (~0,85 GB; misma cabecera y comprobaciones que
         `vk_pipelines.bin`), que mientras se juega solo se lee: una librería se busca ahí sin compilar
         (`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`) y, si no está, se hace en la caché de pipelines
         de siempre, que sigue guardándose en ~16 ms y no en 2,6 s. Con un driver nuevo el fichero no vale y
         Configuración pide volver a hacerla. La caché de disco del propio driver no basta: con las librerías
         compiladas solo en cachés temporales, el paseo tuvo 1,8 s de compilaciones y 50 frames de más de 40 ms.
       - Coste: la primera vez ~3 min (172 s: traducciones 46 s, librerías 125 s; 12 hilos, Ryzen 5 5600X, RTX
         3060); otra vez con todo hecho, ~5 s (el fichero no se reescribe si no hay nada nuevo). Disco: ~1,45 GB
         (librerías 834 MB, SPIR-V 448 MB, traducciones 137 MB, caché de pipelines 17 MB). Memoria: con
         `vk_precompiled.bin` cargado el proceso usa ~0,9 GB más (en el mundo: 5,5 GB de working set frente a 4,6).
       - Memoria al precompilar (2026-10-23): RADV llega a ~1,3 GB por shader en los fragment shaders más grandes
         (41 KB de código Maxwell), y un hilo por núcleo sobre los grandes a la vez pasaba de 9,5 GB en un portátil de
         14 GB (Radeon 660M, 12 hilos; con 7 GB el kernel la mataba). Las librerías comparten un presupuesto (un cuarto
         de la RAM; `SAVAGEWIND_PRECOMPILE_MEM_MB=<n>`): cada una reserva lo estimado por su tamaño (16 MB + 1 MB por
         cada 32 bytes de código), y un hilo coge la más grande que quepa o, si no, la más pequeña. glibc se quedaba lo
         que el driver libera en la arena de cada hilo: `malloc_trim` tras las grandes.
       Paseo con guion (el de arriba, del primer frame del mundo al final), con `SAVAGEWIND_VK_SPIRV_SALT=<n>`, que
       cambia una palabra de la cabecera de cada SPIR-V para que la caché del driver no acierte nada (lo nunca visto
       sin borrar esa caché): sin precompilar, 24,3 s de compilaciones en el hilo de render, 68 frames de más de
       40 ms, 29 de más de 150 ms y un máximo de 4,7 s; con la precompilación, 0,81 s, 10, 1 y 523 ms (ese en la
       carga del mundo; pasada la carga, 5 frames de más de 40 ms y un máximo de 55 ms). Primer frame del mundo a los
       20 s en los dos. Antes de las librerías de pipeline,
       solo con traducciones y SPIR-V precompilados, eran 12,7 s de compilaciones y 51 frames de más de 40 ms. Lo que
       queda: los enlaces y el precalentamiento al cargar el mundo (~500 en los primeros 2 s) y algún enlace suelto
       de 30-70 ms.
     Medición: `SAVAGEWIND_VK_PROF=1 SAVAGEWIND_HITCH_MS=1` registra cada frame con su desglose (traducción, esperas a
     la GPU, compilaciones, presentación, pausa del ritmo); `SAVAGEWIND_MAX_INTERVAL=2` impide que el juego pase a 20
     FPS, así su paso por frame es fijo y un recorrido con guion sigue siempre el mismo camino (comparaciones A/B);
     `SAVAGEWIND_LOG_FRAMETIME=<ms>` da el tiempo de frame que mide el propio juego; `SAVAGEWIND_LOG_HOLD=1` dice qué
     shader dejó el resto de la lista en una pieza. Paseo con guion (Continue, ~90 s caminando, frames 520-3200,
     ventana oculta 1280x720, RTX 3060), con el intervalo dinámico del juego, tres corridas de cada uno alternadas:
     antes 29,5-29,7 FPS, 79 / 79 / 46 frames de más de 40 ms, percentil 99 de 50 ms y 19 / 19 / 13 bajadas a 20 FPS;
     después 29,8-29,9 FPS, 22 / 20 / 22 frames de más de 40 ms, percentil 99 de 38-39 ms y 9 / 9 / 7 bajadas. Quedan: shaders nunca vistos (un pipeline que el driver no tiene en caché
     cuesta 40-100 ms; 193 y 322 ms en un paseo de 5 minutos por zonas nuevas), la ráfaga de pipelines al entrar en
     una zona ya vista (~35 ms en dos frames; ya quitada: precalentamiento, arriba), zonas de 5.000+ draws donde la GPU iba ~15 ms por detrás (un tercio eran
     las copias de feedback, ya quitadas: ver arriba), y picos del
     propio juego (sus hilos de trabajo llenan sus tres núcleos emulados, 60-80 ms; también con la configuración
     anterior).
   - Resolución interna y escalador (2026-10-19): BOTW dibuja su escena 3D en targets de 1600x900 (acoplada) y la
     compone en su imagen final de 1920x1080 (con el HUD). Los render targets se reservan a una de dos escalas
     (`runtime/nvn/vulkan/textures.c`, `tex_scale_group`): la imagen final (1920x1080 y sus mitades, y las imágenes
     que se presentan, flag 0x1) a la resolución de la ventana, así el HUD y los menús salen nítidos a cualquier
     tamaño; los demás targets 16:9 de pantalla (flag 0x8, 2D o arrays 2D) a la resolución interna: la de la ventana
     con Vídeo > Escalador en No (la escena de 1600x900 a 1920x1080 en una ventana de 1080p, a 3840x2160 en 4K),
     dividida por el factor del modo con AMD FSR 1 (Ultra calidad 1,3, Calidad 1,5, Equilibrado 1,7, Rendimiento 2). Con FSR 1 y la escena más chica que su sitio
     en la imagen final, EASU y RCAS corren sobre la escena justo antes del draw del juego que la compone
     (`exec.c` `scene_upscale`), en el formato de la escena (A2B10G10R10, sin bandas), y ese draw muestrea el
     resultado 1:1. Por defecto el escalador está en No (en macOS sigue MetalFX); `settings_version` 3: en un
     archivo anterior "fsr1" solo escalaba la imagen final de 1080p a una ventana mayor, así que carga como No, salvo
     los de versión 1 cuya resolución interna era una parte de la pantalla (75 %, 50 %, 25 %), que siguen con FSR 1
     en el modo más cercano. Lo que faltaba para que BOTW funcionara escalado:
     - Arrays 2D: BOTW dibuja pases de media resolución en capas de arrays de 800x450x2; solo escalaban los 2D, y
       esos pases mezclaban un depth escalado con capas sin escalar y dibujaban un cuarto de la imagen (la costura
       vertical en x≈960 y la mitad oscura de la colina). Los attachments del mismo tamaño en un pase toman el grupo
       del escalado.
     - El borrado de los targets escalados en su primer uso de cada frame (regla de LA): BOTW guarda datos en ellos
       de un frame a otro. Ahora es opcional (`SAVAGEWIND_FRAME_CLEAR=1`); las imágenes nuevas empiezan en cero.
     - Subidas desde la CPU a un target escalado (las miniaturas de las partidas) pasan por una imagen del tamaño del
       juego y un blit. Los offsets de texel de los shaders traducidos van en texels del juego, y los vertex shaders
       también tienen la tabla de escalas (texel fetch de targets escalados).
     - Resolución dinámica: el juego elige su resolución 3D (1600x900 a 1280x720, un target por tamaño) por el
       tiempo de GPU que miden los contadores de `nvnCommandBufferReportCounter`; medían nuestra traducción y el
       mundo abierto bajaba a 1280x720. Ahora todos los contadores de un frame llevan la misma marca de tiempo (sin
       tiempo de GPU) y el juego se queda en 1600x900 (`SAVAGEWIND_VK_COUNTER=live`: como antes). El paso a 20 FPS
       no depende de ellos (usa el cronómetro de CPU del juego).
     Pruebas: `SAVAGEWIND_UPSCALE_AT=<frame>:<off|ultra|quality|balanced|performance>,...` cambia el escalador en
     esos frames (A/B de la misma vista en una corrida), `SAVAGEWIND_SCENE_FSR=0` deja la composición bilineal del
     juego, `SAVAGEWIND_NO_SCALE=1` no escala nada. Una ventana oculta de 3840x2160 funciona aunque el escritorio sea
     más chico (`SAVAGEWIND_WINDOW=3840x2160 SAVAGEWIND_HIDDEN=1`). Metal (`metal.m`) sigue con una sola escala: allí
     los modos quedan en gris.
     Medición (RTX 3060, ventana oculta, mundo abierto y santuario quietos, 300 frames por modo): todos los modos
     mantienen 30 FPS (máximo 34-35 ms) a 1080p, 1440p y 4K; la GPU trabaja 10-13 ms por frame (30-40 % de carga) y
     el driver ajusta su reloj a la carga, así que los ms casi no cambian entre modos: lo que cambia es el reloj y la
     potencia. A 4K: No 1640 MHz / 56 W en el mundo (1377 MHz / 47 W en el santuario), FSR 1 Calidad 1368 / 47 W,
     Rendimiento ~1024 MHz / 34 W; a 1080p todos rondan 1100 MHz / 34 W (la traducción de la lista en la CPU,
     7-9 ms, y el tope de 30 FPS mandan).
   - Frecuencia de frames (2026-10-20): Vídeo > Frecuencia de frames (ajuste `frame_rate`: `off`, 30, 60 por
     defecto, 90, 120) con el reloj del juego igual a cualquier frecuencia. Cómo lleva el tiempo BOTW 1.8.2:
     - El ritmo lo marca la presentación (`present_pace` en `nvn/objects.c`, con el intervalo que pide el bucle de
       frames: 2 = 30 FPS, 3 = 20). La espera de vsync del propio bucle (0x7000d811fc: el evento de vsync y más
       vsyncs hasta 0,99 x intervalo / 60 s del reloj del sistema) solo corre en los primeros frames tras arrancar.
     - El paso de tiempo es `ksys::VFR` (instancia en [0x7001d32010], diseño de `VFR.h` de zeldaret): se inicia con
       intervalo base 2, así que un "delta frame" es 1/30 s. Cada frame `VFR::updateInterval` (0x7001409230) fija la
       razón de intervalo (intervalo / 2: 1 a 30 FPS, 1,5 a 20) y de ahí salen delta frame (razón x el multiplicador
       de cámara lenta más bajo) y delta time (delta frame / 30). Unas 1.600 llamadas los leen: la hora del día
       (`TimeMgr` suma su paso x delta frame; el tiempo está en TimeMgr+0xb8, 360 = 24 h), la luna de sangre, el
       paso de Havok (la física recibe delta time cada frame y lo acumula), la estamina (consumo por segundo x 1/30 x
       razón), animaciones e IA. Unos pocos leen el intervalo entero; el visor de depuración del juego usa
       intervalo 1 (60 FPS) y varios de esos sitios tienen su rama para él.
     - Los menús cuentan frames: mantener una dirección sobre un valor lo cambia cada 6 frames (0,2 s a 30 FPS, 0,1 s
       a 120 sin más). El juego en pausa (menú de pausa, mapa: el estado de 32 bits en 0x7001cb48a4 vale 2, 0
       jugando) actualiza VFR dos veces por frame.
     - Los mandos (`sead::ControllerBase::updateDerivativeParams_`, 0x7000d9d8a0) cuentan cuánto lleva pulsado cada
       botón sumando un paso por frame (un float en 0x7001c4ba40, 1 siempre en BOTW): de ahí la repetición de una
       dirección mantenida y algunas pulsaciones largas.
     Lo que hace Savage Wind (`runtime/hle/framepace.c`, dos parches en `tools/patches.py`):
     - `present_pace` espera el turno de cada presentación en una rejilla fija de 1/frecuencia (un frame tardío por
       menos de un periodo no la mueve; uno más largo la reinicia). Sin límite: 300 FPS como mucho (con pasos más
       cortos los relojes float del juego pierden precisión). La espera del bucle (0x7000d811fc) se salta.
     - Dentro de `updateInterval`, después de que el juego pone su razón y antes de los multiplicadores y los deltas,
       `rt_vfr_update` pone la duración real: razón = segundos x 30 (0,5 a 60 FPS, 0,25 a 120). Cuando las
       presentaciones llevan medio segundo en su turno, un periodo exacto; si no, la media de unos 20 frames; más una
       cincuentava parte de lo que se dio de más o de menos frente al tiempo real, y como mucho un 3 % distinto del
       paso anterior: el juego lee algunos deltas con un frame de retraso y un paso que salta entre frames separa la
       cámara de Link (en la segunda prueba de juego, visto de lado, Link parecía retroceder en su sitio cuando el
       paso alternaba entre el periodo exacto y el medido). Así el tiempo del juego sigue al real. Un paso es 1/20 s como mucho (razón 1,5, la más larga de la consola): un tirón se
       recupera después, pero si se deben más de 0,25 s el juego se ralentiza, como en la consola, en vez de dar pasos
       que rompen la física. Con intervalo fijo pedido por el juego (cargas de escenario), 1 como mucho. El intervalo
       entero (las ramas de 60 del juego con 1) sigue la media de unos 20 frames con histéresis: 1 por encima de
       ~43 FPS, 2 por debajo de ~35, 3 por debajo de ~23; nunca alterna de un frame al siguiente (en la primera prueba
       de juego saltaba entre 1 y 2 en cada tirón y Link parecía ir a trompicones). En pausa (dos actualizaciones por
       frame) la razón es la duración del frame entre presentaciones.
     - El paso de los mandos es la duración del frame entre presentaciones, en frames de consola: lo que se mantiene
       pulsado dura lo mismo que en la consola.
     - En los menús (pantalla de título: la pantalla 1 del gestor de pantallas [0x7001c05d88]; menú de pausa y mapa)
       la frecuencia baja a 30: su ritmo es el de la consola (repeticiones medidas idénticas al juego sin cambios a
       30, 60 y 120). También en los eventos (cinemáticas, conversaciones, fanfarrias: `ksys::evt::Manager`
       [0x7001cd8db8] tiene un evento activo en +0x1d2b8, lo que lee `hasActiveEvent` 0x7000ffa208; la hora del día
       se detiene mientras tanto): algunas de sus esperas cuentan frames (la introducción de una partida nueva daba el
       control 2,1 s antes a 60 FPS y 3 s antes a 120; a 30 en el evento, igual que en la consola). Y mientras hay un
       vídeo (`movie.c`: decodificador de vídeo arrancado): las cinemáticas prerrenderizadas (`Movie/Demo*.webm`)
       son de 30 FPS.
     - La lógica 2↔3 del juego queda con máximo 2 (`SAVAGEWIND_MAX_INTERVAL` vale 2 por defecto).
     - Vsync: Vídeo > Sincronización vertical (ver "VSync y VRR" abajo). Por defecto Rápida (MAILBOX), sin tearing;
       cada refresco muestra la imagen más nueva. Una frecuencia que divide la del monitor (60 en 60/120/240 Hz) es
       la más fluida; una que no (60 o 90 en 144 Hz) alterna 2 y 3 refrescos por imagen; por encima de la del
       monitor las imágenes de más no se ven. La descripción de la opción muestra los Hz de la pantalla
       (`winsize_refresh_rate`).
   - VSync y VRR (2026-10-22, rama `vrr`): Vídeo > Sincronización vertical (ajuste `vsync`: `on`, `fast`, `adaptive`,
     `off`; en el menú F1 también, con la explicación larga). Elige el modo de presentación
     (`runtime/nvn/vulkan/device.c`): Sí FIFO, Rápida MAILBOX (por defecto, lo de antes), Adaptativa FIFO_RELAXED, No
     IMMEDIATE; si la superficie no tiene el modo pedido, Adaptativa cae en Sí, No en Rápida y Rápida en Sí. Cambiarlo
     rehace la cadena de presentación en el momento. El log dice el modo, los que tiene la superficie, si la ventana
     es ventana, pantalla completa sin bordes (el modo del escritorio) o exclusiva (otro modo de la pantalla) y los
     Hz: `[vk] swapchain 2560x1440, 3 images, present mode fifo (...; surface has fifo, fifo relaxed, mailbox,
     immediate, other), borderless fullscreen (the desktop's mode), display 239.77 Hz`. La RTX 3060 (driver 617) da
     FIFO, FIFO_RELAXED, MAILBOX, IMMEDIATE y FIFO_LATEST_READY, y `VK_KHR_present_wait`,
     `VK_EXT_full_screen_exclusive` y `VK_EXT_swapchain_maintenance1`.
     - Con G-SYNC / FreeSync la pantalla espera a la imagen, no al revés, solo si la presentación espera a la
       pantalla: Sí (FIFO). Con Sí y Adaptativa los FPS quedan un poco bajo el refresco (`framepace.c` `vsync_cap`:
       refresco - refresco² / 3600, el tope de NVIDIA Reflex: 59 a 60 Hz, 138 a 144, ~224 a 239,77): a la frecuencia
       del monitor FIFO agrega hasta un refresco de espera y el monitor sale de su rango variable. Sin límite con Sí
       es ese tope. Rápida y No no esperan a la pantalla: sin tope (300 como antes).
     - El ritmo no pelea con la pantalla: una frecuencia a menos del 1 % de un divisor del refresco toma
       exactamente ese divisor (`on_refresh`: 60 en 239,77 Hz es 59,94, una imagen cada 4 refrescos; 30 es 29,97;
       90 queda en 90). Sin eso, en un monitor sin VRR las imágenes se corren contra los refrescos y cada pocos
       segundos una espera un refresco menos o más. Con VRR no cambia nada visible; con No no se aplica.
     - Qué poner en el driver de NVIDIA para G-SYNC (Panel de control de NVIDIA):
       1. Pantalla > Configurar G-SYNC: activar G-SYNC (o G-SYNC Compatible) "para modo de ventana y de pantalla
          completa". Savage Wind en pantalla completa usa el tamaño del escritorio sin bordes (exclusiva solo con
          otro tamaño de la lista), y para el driver eso es una ventana.
       2. Administrar la configuración 3D (para `savagewind.exe` o global): "Método de presentación de
          Vulkan/OpenGL" = "Preferir en capas en la cadena de intercambio DXGI" (lo que deja a G-SYNC, a la
          presentación independiente y al HDR automático ver una ventana Vulkan); "Sincronización vertical" = "Usar
          la configuración de la aplicación 3D"; "Tecnología de monitor" = G-SYNC (Compatible); "Modo de baja
          latencia" en Desactivado o Activado (Ultra también limita los FPS); "Velocidad de fotogramas máxima" no
          hace falta (Savage Wind ya limita).
       3. En Savage Wind: Sincronización vertical = Sí, y la frecuencia que se quiera (sin límite: ~224 a 240 Hz).
       Con AMD: Software Radeon > Pantalla > AMD FreeSync activado, y Sí en el juego. Para comprobarlo: el
       indicador de G-SYNC del panel de NVIDIA (Pantalla > Indicador de G-SYNC) o los Hz del menú del monitor, que
       siguen a los FPS (60 a 60 FPS).
     - Medición (`SAVAGEWIND_LOG_PRESENT=<n>`: cada n presentaciones una línea `[present]` con cada cuánto se
       presentó y, con `VK_KHR_present_wait`, se mostró cada imagen, su desviación, el p99 de lo lejos que queda de
       la mediana, la parte a 2 ms o más, y de presentar a mostrar; `SAVAGEWIND_PRESENT_CSV=<archivo>` escribe cada
       presentación; `SAVAGEWIND_VK_PRESENT=fifo|mailbox|relaxed|immediate` fuerza un modo). Corridas ocultas a
       1280x720 (RTX 3060; Continuar en la Meseta, caminar, esprintar, saltar; parte de ellas con los juegos de otras
       sesiones en la misma GPU), sin los 200 primeros frames:

       | VSync, FPS | entregadas: mediana, desv. sin tirones | mostradas: mediana, desv., dentro de 1 ms | entregar a mostrar |
       |---|---|---|---|
       | Sí, 60 | 16,64 ms, 0,80 | 16,59 ms, 1,02, 84 % | 2,2 ms (desv. 1,7) |
       | Rápida, 60 | 16,65 ms, 0,72 | 16,61 ms, 0,97, 83 % | 2,0 ms (1,2) |
       | Adaptativa, 60 | 16,66 ms, 0,57 | 16,61 ms, 0,75, 90 % | 2,1 ms (1,5) |
       | No, 60 | 16,65 ms, 0,55 | 16,62 ms, 0,78, 87 % | 1,9 ms (1,3) |
       | Sí, 120 | 11,80 ms, 1,41 | 11,81 ms, 1,63, 55 % | 4,3 ms (1,0) |
       | Sí, sin límite | 11,76 ms, 2,74 | 11,80 ms, 2,81, 54 % | 4,1 ms (1,1) |
       | Rápida, sin límite | 11,62 ms, 3,68 | 11,55 ms, 3,74, 49 % | 3,5 ms (1,2) |

       Cuando el juego llega a la frecuencia, las imágenes salen cada 16,6-16,7 ms con menos de 1 ms de desviación
       en los cuatro modos; un tramo sin tirones dio 16,67 ms (desv. 0,21) entregadas y (0,42) mostradas. Los tirones
       (2-3 % de los frames) son del juego (traducción en la CPU). A 120 o sin límite el juego no pasa de ~85 FPS en
       ese sitio y el ritmo sigue a sus tiempos de frame. Con una ventana oculta el driver da la hora de mostrar,
       pero no la de un monitor de verdad: lo que G-SYNC cambia en pantalla (sin cortes ni saltos de refresco a FPS
       variables) hay que verlo en pantalla completa. El reloj del juego no cambia: con Sí a 60 (59,94) y sin límite
       (tope 224), 30,0 frames de consola y 1,00 minuto de juego por segundo, Link a 5,5 m/s, saltos de 0,68 m.
     Medición (corridas ocultas con guion a 1280x720, RTX 3060; Continue en la Meseta, caminar, esprintar hasta
     agotar la estamina, dos saltos; sonda cada 20 ms): con el ritmo propio del juego y a 30, 60, 90, 120 y sin
     límite, el juego avanza 29,97-30,02 frames de consola por segundo (el juego sin cambios 29,5: pierde tiempo en
     los frames lentos), la hora del día 1,00 minuto por segundo, Link camina a 5,49-5,57 m/s y esprinta a
     8,0-8,2 m/s, la estamina baja a ~300-308 por segundo y se recupera a ~294-300, y el salto sube 0,68 m en
     0,51-0,56 s. Ese sitio no pasa de ~45-60 FPS en este equipo (la traducción de la lista de comandos en la CPU);
     el título y los menús sí llegan a 120. La introducción de una partida nueva (voz de Zelda) da las mismas frases
     y el control a 22,90-23,13 s de la primera a 30 y 60. Una rama lanzada (Havok) sigue la misma trayectoria a 30,
     60 y 120 en capturas cada 133 ms.
     Pruebas: `SAVAGEWIND_FPS=<n>` (0: sin límite) en lugar del ajuste; `SAVAGEWIND_FRAMEPACE=0` deja el ritmo y el
     reloj del juego (30 / 20 FPS, como antes); `SAVAGEWIND_MOVIE_FPS=<n>` / `SAVAGEWIND_MENU_FPS=<n>` (0: la
     elegida) durante los vídeos / en los menús; `SAVAGEWIND_EVENT_FPS=<n>` en los eventos;
     `SAVAGEWIND_NO_CONTROLLER_STEP=1` deja el paso de los mandos en 1;
     `SAVAGEWIND_LOG_FRAMEPACE=<n>` cada n actualizaciones el objetivo, la duración, la razón, el intervalo y lo que
     se debe; `SAVAGEWIND_SCRIPT_CLOCK=1` hace que `SAVAGEWIND_INPUT` y `SAVAGEWIND_SHOT_AT` cuenten 30avos de
     segundo y no frames presentados (el mismo guion a cualquier frecuencia); `SAVAGEWIND_CLOCK_PROBE=<ms>` escribe
     cada <ms> una línea `[clock]` con el tiempo real, los frames, el tiempo propio del juego (la suma de sus delta
     frames), la hora del día, la posición de Link (PlayerInfo [0x7001cb2658]+0x88) y su estamina (+0xa4);
     `SAVAGEWIND_LOG_OPTIONS=1` registra cada valor que se guarda desde la pantalla de opciones, con la hora;
     `SAVAGEWIND_LOG_SCREENS=1` el estado de pausa y las pantallas de interfaz que existen;
     `SAVAGEWIND_DUMP_DATA_AT=<frames>` vuelca `.data` y `.bss` de main a `build/data_<frame>.bin` (para comparar
     estados del juego).
   - 60 FPS estables en el mundo abierto (2026-10-20). Dónde se iba el frame (perfilador de muestreo y
     `SAVAGEWIND_TIMELINE`): el hilo principal (0x101) graba ~30 de las ~62 listas del frame y hace el cálculo; dos
     hilos de trabajo graban el resto; el hilo de render (sead, 0x7000d81100) entrega UNA lista por frame
     (`nvnQueueSubmitCommands`, lr 0x7000d81158), pone su fence y presenta, y el hilo principal no empieza a grabar
     el frame siguiente hasta que el de render ha presentado. Tres cosas alargaban el frame:
     - La traducción de la lista (~10 ms en el mundo) iba dentro de `nvnQueueSubmitCommands`, en el camino crítico
       de cada frame. Ahora las entregas, los fences de la cola, las presentaciones y los finalize de texturas y
       syncs van, en su orden, a un hilo propio (`nvn/objects.c`, "the submission queue") y el juego sigue sin
       esperar. Espera donde esperaría a la GPU en la consola: `nvnSyncWait` a que la cola pase el fence (uno
       grabado dentro de una lista: todo lo entregado) y luego a la GPU; `nvnQueueFinish` a todo. Las listas leen la
       memoria del juego cuando se ejecutan, como la GPU de la consola cuando llega a ellas. Como mucho una
       presentación espera en la cola (`SAVAGEWIND_QUEUE_AHEAD=<n>`); `SAVAGEWIND_ASYNC_EXEC=0` lo hace todo en el
       hilo que llama, como antes. El reinicio en el sitio vacía la cola antes de desmontar.
     - BOTW pone FPCR.FZ (denormales a cero) al empezar sus hilos (main 0x7000b4de30 y la entrada de hilos
       0x70000c4e3c), y el aplanado por software mandaba cada operación de coma flotante por el camino lento
       (`simd_fp.h` slow/vslow: ~40 % del tiempo del hilo principal). En x86-64 el MXCSR del hilo sigue ahora a FZ
       (DAZ para las entradas, FTZ para los resultados: `rt_fpcr_write` en `simd.h`, lo emite el lifter en cada
       `MSR FPCR`) y las operaciones van por el camino rápido; FMIN/FMAX y las correcciones de NaN aplanan sus
       operandos a mano. La única diferencia con ARM: un resultado que redondea al menor normal desde abajo puede
       quedarse (ARM detecta el underflow antes de redondear). Cambia `simd.h` y el lifter: hay que regenerar y
       recompilar la librería del juego.
     - CPU sin AVX2 / FMA (#9): en x86-64 el código del juego tiene dos destinos, x86-64-v3 (`GENARCH`) y el
       compatible x86-64-v2 (`GENARCH_COMPAT`, sin `-mfma`). La pantalla de inicio compila el que corre la CPU
       (`gamelib_compat`, `runtime/gamelib.h`), cada uno con su id (`GAMELIB_ID`, `GAMELIB_ID_COMPAT`): una
       librería v3 nunca se carga en una CPU que no la corre, y una carpeta llevada a un PC del otro tipo se vuelve
       a compilar. Sin instrucciones FMA `simd.h` usa su propio fma (`simd_softfma.h`, que sigue al MXCSR como
       ellas; `tools/test_softfma.c` lo compara bit a bit): el de glibc se equivoca con FTZ / DAZ o redondeos
       dirigidos. `SAVAGEWIND_COMPAT_CPU=1` elige el compatible en cualquier CPU (para comparar los dos).
     - Unos ~10 dispatches por frame escriben en la memoria del juego con STG (sus storage buffers, 7-8 por dispatch,
       pequeños, en el heap importado), el primero ~100 draws dentro del frame. Para no copiar en la CPU memoria a
       medio escribir, el resto de la lista iba a la GPU de una vez al final: la GPU esperaba a que se tradujera todo
       el frame y el fence que el juego espera al principio del siguiente llegaba ~traducción + GPU después. Ahora la
       memoria que esos shaders pueden escribir (sus storage buffers) forma un conjunto de rangos; un constant
       buffer dentro se lee en su sitio del heap importado (como lo lee la GPU de la consola, después del shader:
       cada pase empieza con una barrera completa) en vez de copiarse, y cualquier otra copia de CPU de esa memoria
       espera antes a la GPU (0 veces en el paseo). La lista sigue yendo a la GPU por partes. Un shader que escribe
       sin storage buffers (o con el conjunto lleno) retiene el resto de la lista como antes;
       `SAVAGEWIND_VK_GMEM_HOLD=1` retiene siempre.
     - El hilo principal esperaba en `nvnSyncWait` a que la GPU acabara el frame que se acababa de entregar, y solo
       entonces el de render entregaba el siguiente: la GPU se quedaba parada desde el final de cada frame hasta que
       llegaba el primer trozo traducido del siguiente (~5 ms de ~22 en el mundo con una Radeon 660M, y el driver le
       bajaba el reloj de ~1890 a ~1570 MHz). Ahora esa espera va un frame tarde, como en LAS Recomp (3fc84bd en Metal,
       13398126d66a en Vulkan): esperar el fence N espera el que se dejó la vez anterior y vuelve, así que como mucho
       hay dos frames de GPU en cola y nunca se aplaza un fence ya alcanzado (`renderer_wait_seq`). Lo que el juego
       escribe mientras es su frame siguiente, que tiene doble búfer como en la consola; los constant buffers se copian
       al traducir. Los render targets que lee la CPU (cielo, exposición, ocho de 128x128 por turnos) llegan un frame
       después, salvo uno que no se ha leído en los últimos 16 frames (la foto de la cámara): la espera sobre su envío
       es completa. Medido con 2+2 pasadas alternas (Kakariko, establo y Hateno, sin límite de FPS, 1280x720): 40,1 →
       54,0, 47,8 → 63,6 y 51,5 → 59,0 fps; la GPU ocupada del 74-83 % al 98-99 %. Sin saltos de brillo en 60 capturas
       seguidas de día, de noche, con lluvia y dentro de Vah Ruta, y la foto de la cámara correcta.
       `SAVAGEWIND_GPU_OVERLAP=0` lo quita.
     Además:
     - Copias de constant buffers: se comparten en toda la lista (antes, solo dentro de cada trozo de ~300 draws que
       va a la GPU) y `UpdateUniformBuffer` (~450 por frame en el mundo, sobre solo ~40 rangos pequeños) invalida solo
       las copias que tocan su rango (antes, todas). Lo que más se copiaba era un buffer de 64 KB que el
       shader indexa con un registro (se liga entero) y que `UpdateUniformBuffer` cambia 16 bytes entre draws
       (~220 copias, ~14 MB por frame; ~94 % de los bytes de constant buffers copiados en Kakariko, el establo y
       Hateno). Es el vertex shader que elige la cascada de sombra (`agl_technique`): recorre las cascadas con un
       contador y lee menos de 400 bytes. El traductor (`cbuf_reach` en `translate.c`) recorre el programa con los
       valores de registro que se conocen al traducir (constantes movidas, sumadas, desplazadas; los dos caminos de
       un salto con condición desconocida) y, si todas las lecturas indexadas de un cbuf tienen el registro
       conocido, `cbuf_size` es hasta dónde llegan y el cbuf deja de estar en `cbuf_dynamic`: se copia solo eso.
       Lo que escribe cada instrucción sale del código que se emite para ella, así que lo que no se modela queda
       desconocido (se liga entero, como antes). En BOTW se acotan 46 shaders de `agl_technique` (de 64 KB a
       512-1024 bytes); la copia de memoria del juego baja de ~440 a ~40 MB por segundo.
     - Prioridades de los núcleos: Horizon da el núcleo al hilo de mejor prioridad en cuanto puede correr. El código
       generado no tiene puntos de expulsión propios y el núcleo solo cambiaba de manos en la siguiente syscall del
       que lo tenía: una vez por segundo un hilo de trabajos de BOTW (prioridad 48, núcleo 2) corre ~5 ms de código
       y un hilo de prioridad 44 del mismo núcleo, al que el principal espera, esperaba el núcleo (un frame ~10 ms
       más largo cada segundo). Ahora `rt_dispatch` (cada llamada indirecta, un punto seguro: todo el estado está en
       `Cpu`) cede el núcleo si espera uno mejor (`kernel_preempt`, `g_core_best_wait` por núcleo); son menos puntos
       de expulsión de los que tiene la consola. `SAVAGEWIND_NO_PREEMPT=1` lo quita. Queda una espera del hilo
       principal en un mutex del juego que tiene ese trabajo de 1 Hz (Horizon heredaría la prioridad; aquí no).
     Medición (Meseta, guion de paseo hasta la caída del acantilado, ventana oculta de 1280x720, RTX 3060 y Ryzen 5
     5600X sin otro juego ni compilador corriendo; `SAVAGEWIND_FPS=60`; varias corridas de cada uno):
     - Antes (cola apagada, FZ por software, retención): 51,5-56,8 FPS, p50 16,7-17,8 ms, p90 19,6-24,1 ms, p99
       38-43 ms, 67-161 frames de más de 25 ms en ~36 s.
     - Con la cola, FZ en MXCSR y los rangos: 59,2-59,5 FPS, p90 17,0-17,1 ms, p99 29-34 ms, 44-53 frames > 25 ms.
     - Más la cesión de núcleos: 58,5-60,0 FPS, p90 17,0 ms, p99 17,4-20,6 ms, 4-12 frames > 25 ms (sin ella, en la
       misma tanda: p99 23,6-24,8, 19-22).
     - Ventana de 2560x1440 (más GPU: ~8 ms por frame): 59,8-60,0 FPS, p90 17,0 ms, p99 18-24 ms (antes 57,1-57,2
       FPS, p90 18,5, p99 36,8).
     - Sin límite: 97-100 FPS (p50 9-10 ms); a 120: 84-86 FPS (medido antes de la cesión de núcleos y de compartir
       las copias en toda la lista). El límite ahora es el hilo de la cola (~9 ms de
       traducción por frame a ~100 FPS: descriptor sets, llamadas al driver y el trabajo por draw); la GPU ~6 ms
       por frame; el hilo principal hace ~6-7 ms de trabajo propio y espera el resto.
     - Reloj y física a 60 iguales que antes (juego 30,00 frames de consola por segundo, hora 1,000 min/s, caminar
       5,54 m/s, esprintar 8,13 m/s, estamina -304/+300 por segundo, saltos de 0,68 m); guardar desde el menú de
       pausa (miniatura y `caption.jpg`), menú de pausa y mapa, la introducción de una partida nueva y el reinicio en
       el sitio funcionan; 0 pérdidas de dispositivo.
     Segunda ronda (2026-10-21, tras la prueba del usuario: 50-60 FPS con caídas a 45). Su `savagewind.ini` tenía
     `frame_rate = 90` (no 60), pantalla completa de 2560x1440, SMAA y overlay completo; y las cachés de shaders y
     pipelines estaban frías para ese ejecutable (el id del traductor es la hora de compilación de `translate.c`:
     tirones de hasta 1,9 s la primera vez que aparece cada efecto). En la parte pesada del paseo (~3500 draws por
     frame) la traducción del hilo de la cola es el límite: ~3,7 µs por draw. Recortes en ese hilo:
     - `vk_wait_seq` no llama a `vkWaitSemaphores` si la GPU ya pasó ese punto: reciclar un slot de entrega
       (`sub`), cuyo trabajo había terminado hacía frames, costaba ~6 % del hilo dentro del driver de NVIDIA.
     - La caché de descriptor sets distingue las entradas por dos hashes de 64 bits del contenido en vez de guardar
       y comparar la clave (~700 bytes por entrada en una tabla de 3 MB: el `memcmp` era la línea más cara).
     - El hash del fragment shader (512 bytes byte a byte, para listas de omisión y registros) se memoriza por
       dirección de código; las muestras de cambio de texturas leen 8 bytes en hasta 2048 sitios en vez de un byte
       en 4096 (un fallo de caché por byte); prefetch de los registros de comando (los graba otro núcleo).
     - `UpdateUniformBuffer` que escribe los mismos bytes (~1 de cada 5) no invalida las copias.
     Medición (2560x1440, SMAA, overlay completo, alternando con el ejecutable anterior; entre tandas varía ~4 %):
     a 60, 58,5-59,3 → 59,6-59,7 FPS, p90 17,2-17,9 → 17,0 ms, frames de más de 20 ms en ~36 s 72-99 → 47-50; a 90,
     72,0 → 75,4 FPS; sin límite, 75-79 → 79-85 FPS, p90 17,2-17,9 → 15,5-16,8 ms. Lo que queda a 60: crear cada
     pipeline la primera vez que se usa en la sesión (con `async_shaders = 0` el draw espera: ~1,3 s repartidos en
     los primeros segundos en el mundo, luego 50-250 ms cuando aparece un efecto nuevo; mucho más con las cachés
     frías de un ejecutable nuevo).
     Probado y descartado: barreras entre pases solo cuando hacen falta (el pase depende de algo escrito desde la
     última). Saltaba ~70 de ~215 por frame sin diferencia medible: las que hacen falta (un pase lee lo que escribió
     el anterior) son la mayor parte del coste. Quitarlas todas (imagen incorrecta) daba ~15 %.
     Depuración: `SAVAGEWIND_TIMELINE=<frame>:<n>` escribe líneas `[tl]` (ms, hilo, evento) de esos frames:
     entregas, fences, esperas de sync, presentaciones, bloqueos del kernel de más de 0,2 ms, grabación de listas y
     el trabajo del hilo de la cola (pesado: los frames se alargan); `SAVAGEWIND_FRAME_LOG=1` una línea `[ft]` por
     presentación (ms, frame, frecuencia objetivo), `=2` además una `[fsplit]` con cuándo pasó cada paso (ligera);
     `SAVAGEWIND_SAMPLE` también muestrea el hilo de la cola (handle 0xf000); `SAVAGEWIND_LOG_CORES=1` el núcleo y la
     máscara que el juego da a cada hilo.
   - Hacia frames de 8 ms (2026-10-22, rama `frame-8ms`). Un recorrido más parecido a jugar que el paseo: desde la
     partida 0, 60 s corriendo con giros largos de cámara a ambos lados, golpes con la rama, saltos, esprint y fijar
     objetivo (Link corre en curvas por pasto, bosque y rocas, trepa una roca y un árbol): 3500-4900 draws por frame
     (el paseo llega a ~3500). A 2560x1440 con SMAA la GPU hace ~9 ms de trabajo por frame: a 120 FPS el límite es
     ella (el hilo principal espera a la GPU ~27 % del tiempo).
     - Herencia de prioridad en los mutex del juego, como en Horizon: quien tiene un mutex corre con la mejor
       prioridad de los que lo esperan (y eso sigue por una cadena de esperas); al soltarlo, o con
       `SetThreadPriority`, vuelve a la suya. Una vez por segundo el hilo principal (prioridad 44) esperaba un mutex de
       un hilo de trabajos de prioridad 48, que esperaba su núcleo detrás de hilos peores que el principal. A 60 FPS
       en el recorrido: frames de más de 25 ms por minuto 20-35 → 0-6, p99 23,6-25,0 → 19,0-19,6 ms.
       `SAVAGEWIND_NO_PRIO_INHERIT=1` la quita.
     - Uniform buffers dinámicos en los programas de draws: los constant buffers del juego (c3 en adelante, primero los
       del vertex shader, hasta el límite del layout: 15 en NVIDIA) llevan el offset de su copia en
       `vkCmdBindDescriptorSets` y no en el set; como las copias cambian de sitio casi en cada draw (constantes por
       objeto), los sets eran distintos en cada draw (~4000 nuevos por frame). Ahora los comparten los draws con las
       mismas texturas y tamaños de copia: ~500 000 → ~100 000 sets nuevos cada 2 s; a 120, traducción por frame
       8,9 → 7,9 ms. Cambian los layouts de los pipelines (se rehacen una vez). `SAVAGEWIND_VK_NO_UBO_DYNAMIC=1`.
     - El programa y el pipeline del draw anterior se reutilizan mientras no llegue un comando que cambie lo que
       leen sus claves (programa, blend, máscaras, multisample, polígono, atributos y streams de vértices, targets,
       depth clamp) ni empiece un pase: ~1,4 KB de claves menos por draw (~1,5 % de la traducción).
       `SAVAGEWIND_VK_NO_KEY_GEN=1`. El memo del hash de fragment shaders usaba solo 32 de sus 512 entradas (código
       alineado a 256 bytes).
     - `ICommonStateGetter` 50, que BOTW pide cada frame, se registra una vez (escribir la línea era ~1 % del hilo
       principal).
     Todo junto contra `main` (recorrido, 2560x1440 con SMAA, dos corridas de cada uno, alternadas): a 60, p99
     25,1-25,8 → 18,6-19,1 ms y frames de más de 20 ms por minuto 70-81 → 22-30; a 120, 71,7-71,9 → 78,8-80,0 FPS,
     p99 30,8-31,7 → 19,3-19,6 ms, traducción por frame 8,9 → 7,5 ms; sin límite, 71,0-72,0 → 74,5-76,7 FPS. A
     1280x720: a 120, 82,5 → 86,7-89,0 FPS, p99 28-32 → 16-17 ms. Velocidad del juego y física iguales a 60 y a 120
     y a las de `main` (30,00 frames de consola por segundo, hora 1,000 min/s, caminar 5,54 m/s, esprintar 8,02 m/s,
     estamina -300/+300 por segundo, saltos de 0,68 m).
     Lo que queda, medido (cola a 120 tras estos cambios): `draw_` ~60 % del hilo (descriptor sets 19 %, llamadas al
     driver 13 %, el propio `draw_` 12 %, inicio de pases 6 %, texturas 5 %); el primer acceso a cada registro de
     comando ~3 % (los escribió otro núcleo). El hilo principal: ~45 % código del juego (~6 ms por frame, repartido:
     ninguna función pasa del 1 %; `rt_dispatch` 4 %), el resto esperas. `SAVAGEWIND_VK_LIST_STATS=1` (medir) mostró
     que un frame no se parte en listas independientes: ~2800 `CallCommands` por frame, ~1 draw cada una, todas usan
     estado puesto antes. Bajar la traducción a 4-5 ms pide traducir en paralelo (notas de la sesión).
   - Sombras del sol con depth clamp (2026-10-20): `nvnCommandBufferSetDepthClamp` no hacía nada. BOTW lo activa para
     sus tres cascadas de sombra del sol (un array D16 de 896x896x3, ~650-750 draws por frame) y para la consulta de
     oclusión del sol: lo que está más cerca de la luz que el plano cercano de una cascada se aplasta contra él en vez
     de recortarse. Recortado, el risco que cubre el Santuario de la Resurrección no hacía sombra y el sol entraba
     (franjas blancas en el piso, paredes y niebla iluminadas); la primera vez casi no se notaba porque dentro del
     santuario rige una paleta de interior con un sol más débil (luz 4 / 3,6 / 3,4 y niebla cálida), pero al volver
     a entrar ya rige la del exterior (7,5 / 6,9 / 4,7, niebla verde azulada). Las cascadas siguen a la cámara, así
     que lo recortado (las copas de los árboles, por encima de la vista) cambiaba al girarla: las sombras de los
     árboles aparecían, se cortaban y se movían. Ahora el comando se graba (`CMD_SET_DEPTH_CLAMP`) y el pipeline lleva
     `depthClampEnable` (`PipeState.depth_clamp`, en lo que era relleno: las listas de pipelines anteriores lo leen
     apagado). `SAVAGEWIND_NO_DEPTH_CLAMP=1` lo ignora (A/B). Metal (`renderer.m`): `setDepthClipMode`, sin compilar.
     La paleta de entorno (`fcb4` del shader de la máscara de sombras) pasa de la de interior a la de exterior en 60 s
     al salir del santuario y al cargar una partida afuera, y no vuelve a la de interior al regresar (ver abajo).
   - Barreras por imagen entre pases (2026-10-26): cada pase empezaba con una barrera global (`ALL_COMMANDS`,
     escrituras de memoria → lecturas y escrituras). Tras escrituras de attachments, RADV convierte una barrera global
     en escribir e invalidar toda la L2, porque sin imagen no sabe si eran coherentes con ella
     (`radv_src_access_flush` añade `AC_BARRIER_INV_L2`); con barreras por imagen no (`radv_image_is_l2_coherent`).
     Si desde la última barrera completa solo hubo pases (ni copias, ni dispatch, ni subidas, ni escrituras a la
     memoria del juego, ni lecturas de targets lineales: todo eso va tras una barrera completa, que marca
     `bar_other`), la barrera del pase siguiente nombra las imágenes de esos pases (las de sus vistas de attachment,
     tras `vk_view`) más una dependencia de ejecución para lo demás; una imagen diferida fuerza la completa.
     `SAVAGEWIND_VK_FULL_PASS_BARRIER=1` vuelve a la barrera completa. La validación de sincronización
     (`VK_LAYER_VALIDATE_SYNC=1` con `VK_LAYER_SYNCVAL_SHADER_ACCESSES_HEURISTIC=1`, mensajes con
     `SAVAGEWIND_VK_VALIDATION=1`) da en las tres escenas los mismos avisos que antes (los del feedback loop del
     terreno, dentro del pase) y ninguno entre pases; sin barreras de memoria entre pases da cientos. Medido sobre
     `test` (36a02bc, con #108), 3+3 pasadas alternas sin límite de FPS a 1280x720 en una Radeon 660M: GPU por frame
     −1,9 % en Kakariko, −1,1 % en el establo del río y −1,9 % en la puerta de la Ciudad Gerudo; FPS +1,3 % en
     Kakariko (los otros dos, +1,1 y +0,8 %, dentro del ruido).
   - Fallos gráficos que quedaban en la Alpha 2 (2026-10-22):
     - Consulta de oclusión del sol: `ReportCounter` con `SAMPLES_PASSED` (tipo 1) devolvía siempre 0 muestras y
       `ResetCounter` no hacía nada. BOTW dibuja una vez por frame el disco del sol con prueba de profundidad y sin
       escribir color (main 0x7000ea74a8, tres ranuras de resultado por turno, leídas dos frames después) y con eso
       decide su resplandor: con 0 el sol contaba siempre como tapado. Ahora cada draw entre `ResetCounter` y
       `ReportCounter` va dentro de una consulta de oclusión de Vulkan y, cuando termina su envío, {muestras, marca de
       tiempo} va a la memoria del juego, en píxeles del juego (dividido por la escala del render target). Mirando al
       sol desde el acantilado del santuario: 276 muestras. `SAVAGEWIND_NO_OCCLUSION_QUERY=1`: 0, como antes;
       `SAVAGEWIND_LOG_OCCLUSION=<n>` registra los n primeros resultados. `ReportValue`,
       `SetRenderEnableConditional` y `SetRasterizerDiscard` siguen sin hacer nada (se registran la primera vez).
     - Miniatura negra al abrir otra vez el diálogo de guardar: la primera vez el juego copia la imagen final a una
       textura de 256x144 (1920x1080, 854x480, 256x144; la lee con `CopyTextureToBuffer` para el JPEG); al cerrar el
       diálogo la textura se finaliza, y al abrirlo otra vez la hace de nuevo en la misma memoria y la muestra sin
       volver a copiar: en la consola los texeles siguen en esa memoria. Aquí la imagen de Vulkan se liberaba a los 4
       frames sin uso, y la nueva empezaba en negro. Ahora una imagen que dibujó la GPU (sin copia en la memoria del
       juego) se guarda después de esos 4 frames, con una suma de la memoria del juego que cubre, hasta 192 MB (las
       más viejas se van primero; ninguna de más de 16 MB); una textura hecha otra vez ahí la recupera si esa memoria
       no cambió (si cambió, la CPU la reusó: imagen nueva). `SAVAGEWIND_NO_KEEP_IMAGES=1`: como antes;
       `SAVAGEWIND_LOG_KEPT=1` registra las recuperadas. También vuelven así los paneles de la interfaz que el juego
       dibuja una vez en su propio render target (170x40, 85x20...).
     - Con "Shaders asíncronos" (ajuste `async_shaders`, el del usuario), un draw cuyo programa o pipeline se estaba
       construyendo se salteaba hasta el frame siguiente. Dos consecuencias:
       - La imagen "posterizada" tras Continue (cielo y pasto planos, frames 650-900): faltaban el pasto denso y capas
         de nubes mientras sus shaders se construían. Pasaba también con las cachés calientes: un programa nuevo
         esperaba su turno en el hilo traductor aunque su traducción estuviera en memoria y sus módulos y pipeline ya
         hechos (el precalentamiento): ~7.400 draws saltados en los 2 s en que aparece el mundo. Ahora, si la traducción
         está en memoria (`vk_translate_peek`) y el programa y el pipeline están hechos, quedan listos en el acto: 19
         saltados. Con las cachés frías (después de cada actualización, que cambia la versión del traductor) se siguen
         construyendo en otros hilos.
       - El fondo del menú de pausa negro: el juego difumina una captura de la escena una sola vez al abrir el menú, y
         si esos draws se salteaban el fondo quedaba negro (también la primera pausa de cada sesión, no solo tras
         "Salir a la pantalla de inicio"). Ahora un draw a render targets en los que ningún pase
         dibujó el frame anterior (capturas, la miniatura, targets nuevos) espera a sus shaders, como en modo síncrono.
         Con cachés frías eso concentra la construcción en el primer frame del mundo (2,2 s en una prueba, detrás de la
         carga) y deja ~2.700 draws saltados en los 4 s siguientes en lugar de ~29.000.
     - La paleta de 60 s: los valores de luz y niebla de la paleta (`fcb4` del shader 8c42c9bc921ed716, luz 4 / 3,6 /
       3,4 y niebla cálida en el santuario; 7,5 / 6,86 / 4,70 y niebla verde azulada afuera) van de una a otra en línea
       recta en 60 s de tiempo real (1/1800 por frame a 30 FPS, 1/3600 a 60): el paso sigue al reloj del juego. Los
       copia al uniform main 0x7000e90688 (llamado desde 0x7000e8e3bc) desde una tabla de entradas de 0x5c bytes;
       quién la llena no se ha encontrado. Los relojes que usa el juego (`CNTPCT` a 19,2 MHz, delta de `ksys::VFR`)
       andan bien, así que no se ve una causa en la emulación; que en la consola tarde lo mismo no está comprobado.
     (1/1800 por frame) al salir del santuario y al cargar una partida afuera, y no vuelve a la de interior al
     regresar: no se sabe aún si es el comportamiento del juego.
   - Filtrado anisotrópico y enfoque (2026-10-22): Vídeo > Filtrado anisotrópico (No, 2x, 4x, 8x, 16x; ajuste
     `anisotropy = game|2x|..|16x`) fija la anisotropía máxima de los samplers de texturas del juego
     (`vulkan/textures.c` `sampler_aniso`): solo los que filtran lineal entre niveles de mip (suelo, paredes, caminos);
     no los de filtro de punto, los sin mipmaps (la interfaz, targets releídos), los fijados a un nivel (las cadenas
     de bloom y desenfoque) ni los de comparación de profundidad (sombras). Con No queda la del juego (BOTW usa 1, y 2 en
     unos pocos). Un VkSampler por ajuste distinto, compartido entre entradas de pool (`SAVAGEWIND_LOG_SAMPLERS=1`
     lista cada uno). Vídeo > Enfoque (AMD CAS) (No, 10-100 %; `sharpening = 0..100`): AMD FidelityFX CAS (MIT) sobre
     la imagen 3D en la composición de la escena (`exec.c` `scene_post`), después de EASU+RCAS con FSR 1 o sobre la
     escena a la resolución de la ventana con el escalador en No, antes de que el juego dibuje el HUD encima. Pruebas:
     `SAVAGEWIND_FILTER_AT=<frame>:<anisotropía 0-4>/<enfoque 0-10>,...` (A/B de la misma vista),
     `SAVAGEWIND_SCENE_CAS=0`. Sin filas en macOS (Metal no tiene ninguno de los dos).
     El draw toma la copia procesada con el swizzle de la textura del juego (`vk_sample_swizzle`): el menú rápido de
     armas (cruz derecha) compone su captura desenfocada de la escena (1280x720 RGBA8, un target de escena) con
     un shader de interfaz que fuerza el alfa a 1 (swizzle R,G,B,1) y mezcla por alfa. La copia con swizzle identidad
     daba alfa 0 y el fondo quedaba negro con el enfoque encendido (2026-10-22).
   - Resolución de sombras (2026-10-22): Video > Shadow resolution (ajuste `shadow_resolution` = `console` | `high`
     | `ultra` | `extreme`) crea las cascadas del sol a 2048, 4096 u 8192 por lado en vez de 896 (nunca más que el
     `maxImageDimension2D` de la GPU). `textures.c`: grupo de escala
     `RS_SHADOW` para las texturas de profundidad cuadradas de 512 o más que son render targets (el 192x192 de
     profundidad que BOTW también usa no es un mapa de sombras); su escala es el tamaño elegido / el ancho del juego,
     nunca menos de 1 (`output_scale.c` `shadow_map_size`, aplicado en cada presentación: cambiarlo rehace el mapa).
     Viewports, scissors, copias y el `rscale` de los shaders ya siguen la escala de cada textura, como con los
     targets de la escena. El filtro: los tres pases diferidos de la máscara de sombra del sol hacen un PCF de 4
     muestras a ±0,5 texel con el tamaño de texel que el juego pasa en un constant buffer (`c[4]` palabras 212-213 =
     1/896); sin tocarlo, las muestras quedan a un texel de consola (2,3 o 4,6 texels del mapa grande) y la penumbra
     se ve en escalones. En los draws que muestrean un mapa agrandado, `cbuf_info` lo cambia por 1/2048, 1/4096 u 1/8192 en
     una copia del buffer del fragment shader (`sdw_patch`: busca el par exacto; ~10 por frame). El depth bias no se
     escala: la parte por pendiente ya depende del tamaño del píxel del mapa, y la constante (y el -0,002 que restan
     los shaders) es la de consola, que solo puede sobrar. Depth clamp sin cambios.
     Lo demás que lee las cascadas: un downsample a 448x448x3 R16 (`textureGather`, el máximo de cada 2x2) y shaders de
     un solo tap sin offsets. El agua y las demás superficies forward (~20 fragment shaders) toman sus sombras de ese
     downsample, con una sola muestra y coordenadas normalizadas: también va en `RS_SHADOW` (array 2D R16 cuadrado de
     3 capas, sin flag de render target), escalado con el mismo factor que las cascadas (Extreme: 4096x4096x3). Sin
     él, el agua seguía a resolución de consola (#101, 2026-10-25). La regla pide las 3 capas: una R16 de 1024x1024x8
     que se rellena al cargar también casaba, y al cambiar el ajuste jugando se rehacía vacía y desaparecían el
     terreno y la hierba. Hay shaders con offsets fijos de ±1/4096 (sin constant buffer) para
     otro mapa de sombras que no aparece en la meseta: si es de 2048, en Ultra quedaría con escalones (sin verificar).
     Pruebas: `SAVAGEWIND_SHADOW_AT=<frame presentado>:<console|high|ultra|extreme|ancho>,...` (A/B en una corrida),
     `SAVAGEWIND_NO_SHADOW_TEXEL_FIX=<frame presentado>` (el tamaño de texel del juego desde ese frame).
     Costo (RTX 3060, ventana oculta de 1920x1080, meseta, mediana de 10 frames con `SAVAGEWIND_GPUPROF_AT`): los
     pases de las cascadas 0,78 ms en Console, 0,96 en High (+0,18) y 1,48 en Ultra (+0,70); el resto del frame no
     cambia de forma medible. Memoria de video de las cascadas: 4,8 MB, 25 MB, 100 MB y, con Extreme (8192, 2026-10-23), 384 MB. Extreme mide 3,3 ms en los pases de las cascadas (Console 0,77, High 0,93, Ultra 1,41 en la misma corrida),
     sin acné ni sombras despegadas en la meseta ni en el Santuario de la Resurrección. Los perfiles de GPU dan ahora
     el tamaño del depth de cada pase (`pass: ... depth 896x896`).
   - Pantallas anchas y campo de visión (2026-10-24, issue #65): Vídeo > Llenar pantallas anchas (`wide_view`, Sí
     por defecto) y Vídeo > Campo de visión (`field_of_view`, 70-150 %), en Opciones del juego y en Configuración.
     Solo ventanas más anchas que 16:9 (21:9, 32:9); las más estrechas (16:10, 4:3) siguen 16:9 con bandas: el
     texgen de los paneles del HUD (el minimapa) no sigue una vista más alta.
     - El mundo: `output_scale` (con `reshape`) agranda todo render target 16:9 a la forma de la ventana, y el juego
       dibuja en ellos con sus vistas ensanchadas. `tools/patches.py` engancha las proyecciones de sead:
       PerspectiveProjection (vtable +0x58, la carga de tan(fovy/2) y aspect en `+0xac`/`+0xb0`, site `persp_matrix`;
       su desplazamiento `+0xb4`, `persp_matrix_offset`, que el Link del inventario usa, x / forma; y el paso pantalla
       → cámara, vtable +0x68, `persp_unproject_x`/`_y`) y FrustumProjection (`frustum_lr0/1`, `frustum_tb0/1`).
       `output_scale.c` `rt_persp` / `rt_frustum` / `rt_persp_offset` multiplican por la forma toda vista 16:9.
       `proj_update` da la matriz por sucia siempre (sead la recalcula al pedirla): las capas del HUD hechas al
       arrancar (el cursor de los menús) siguen la forma actual. Las ortográficas no se tocan (la composición de la
       escena usa una de 1920x1080; ensancharlas desplazó el menú rápido y la carga, probado).
     - El HUD (`exec.c` `hud_patch`, en el c3 del vertex de los draws al target final, `RS_OUT`, que empieza por una
       matriz de perspectiva simétrica: el bloque de vértices de nn::ui2d, matriz c3[0-3], vista c3[4-6], rectángulo
       del panel c3[33]): una capa cuya matriz el juego hizo sin sead (el cursor de selección) recibe la ensanchada;
       un panel de una capa de 40° que cubre todo el ancho 16:9 (fundidos, el negro de la carga, el fondo borroso de
       los menús) recibe la 16:9, que lo estira a la ventana; un panel entero fuera del 16:9 (partes del layout que en
       la consola no se ven) no se dibuja (`hud_clip`). Los scissors parciales del target final se comprimen igual
       (`scissor_rect`: la máscara del minimapa).
     - Menús: con el estado de pausa 2 (páginas del menú de pausa y mapa, `framepace_menu_ui`) se presenta el centro
       16:9 de la imagen con bandas (`OutScale.crop`), igual que en una pantalla 16:9; el menú rápido (estado 4) y la
       pantalla de carga llenan la ventana. La pantalla de título y sus menús (`framepace_title_ui`, pantallas 1 y
       0x38) no se ensanchan.
     - El FOV escala solo la cámara del mundo (far 25000; HUD, modelos de menú y título tienen 10000), nunca en eventos
       (`framepace_in_event`), con transición de 1/4 s. `SAVAGEWIND_LOG_PROJ=1` lista cada tipo de vista y los cambios
       de FOV; `SAVAGEWIND_NO_HUD_FIX=1` quita `hud_patch`. Pruebas: `SAVAGEWIND_WINDOW_SHOT_AT=<frame>,...` guarda la
       ventana tal cual se presenta (bandas y menús incluidos) en `build/frames/window_N.png`. Metal pasa `false`.
   - Overlay de rendimiento (F10: off, FPS, full; ajuste `perf_overlay`): FPS y ms del juego con gráfico, CPU (total
     y del juego), RAM, GPU (uso, temperatura, potencia, reloj, VRAM) por NVML, que trae el driver de NVIDIA
     (cargado en tiempo de ejecución), y en Linux amdgpu por sysfs. Temperatura de CPU: hwmon en Linux; en Windows
     solo la zona térmica ACPI (este equipo no la tiene): la del encapsulado necesita un driver de kernel, que Savage
     Wind no instala. `runtime/perfmon.c`.
   - Idiomas (2026-10-16): pestaña Language del menú, ajustes `text_language` (región-idioma de BOTW: USen, EUes,
     JPja, ...; `auto`: el idioma de la interfaz del sistema, inglés si el juego no lo tiene) y `voice_language`
     (carpeta de `Voice/`; `text`: las del idioma del texto). El texto es lo que responden `am`
     `GetDesiredLanguage` y `set` `GetLanguageCode` (es-419 carga `Pack/Bootup_USes.pack` y `TitleBG_USes`). Las
     voces se redirigen al indexar el RomFS (`voice_redirect` en `runtime/services/fs.c`): cada
     `Voice/<idioma>/<ruta>` salvo `Common` toma los datos de la misma ruta en la carpeta elegida (los `.bars` y los
     diálogos `Stream_*/…bfstm`, 6441 archivos con inglés), parcheando solo la tabla de archivos en la copia que ya
     usa el overlay de mods. Ambos se leen al arrancar el juego: en juego, "Restart now" (reinicio en el sitio)
     los aplica.
     Tailandés (2026-10-22): la 1.9.0 lo trae (`Pack/Bootup_THth.pack`, `Font/Font_TH.sbfarc` y cuatro fuentes
     `.sbfotf`) y lo elige cuando `GetDesiredLanguage` responde `th` (la 1.9.0 compara el código con "th"; la 1.8.2
     no). Es la última entrada de `g_text_languages` (región `THth`); `language_text_count` / `language_text_index`
     solo lo ofrecen si `extracted/romfs/Pack/Bootup_THth.pack` existe, y con archivos 1.8.2 un ajuste en tailandés
     se guarda pero vale como Auto. No hay voces en tailandés: con "Igual que el texto" el juego toma las inglesas
     (`Voice/USen`).
     - Tamaños de recurso (2026-10-22, issue #2): el juego reserva la memoria de cada recurso según la entrada de su
       ruta en `System/Resource/ResourceSizeTable.product.srsizetable` (RSTB: el tamaño del archivo más lo que arma
       con él, ~850 bytes en un banco de voz). Un banco redirigido más grande que el de la ruta se queda sin memoria:
       el objeto de 0xa8 bytes que lo parsea (tras comprobar `BARS`) recibe NULL y el juego escribe en él
       ("accessing 0x10"). Con texto pt-BR y voces JPja pasaba siempre en la pantalla de título, que carga los
       `Demo*.bars` (`Demo131_0`: 3064 bytes en JPja, 2616 en USes); con en-US no se vio en el título, Continuar,
       Kakariko ni el prólogo. `rstb_patch` (`fs.c`) sirve desde memoria, como `Version.txt`, una copia de la RSTB
       en la que las entradas de los archivos redirigidos que crecieron crecen lo mismo (redondeado a 32, más 32):
       3428 archivos más grandes con JPja, 242 de ellos con entrada (los diálogos `.bfstm` no tienen). La copia va
       en Yaz0 de solo literales (~593 KB por 467 KB). Si el overlay de mods trae su propia RSTB, se deja la suya.
   - Menús traducidos (2026-10-16): la pantalla de inicio y el menú del juego siguen al idioma de texto (Auto: el
     del sistema). `tr("texto en inglés")` (`runtime/ui/i18n.mm`) busca el texto en la tabla del idioma,
     `runtime/ui/i18n/<idioma>.inc` (`T("English", "...")`; `""` deja el inglés). Los widgets de
     `option_rows.mm` y los helpers de `menu.mm` / `launcher.mm` traducen etiquetas, valores, descripciones y
     títulos ellos mismos; los IDs de ImGui siguen en inglés. Mayúsculas en UTF-8 (`tr_upper`: latín, griego,
     cirílico). Ruso, japonés, coreano y chino suman una fuente del sistema con solo los glifos que usan sus
     textos (`ui_merge_fallback_font`), y cada contexto rehace sus fuentes al cambiar el idioma.
     `tools/i18n_check.py` lista los textos que le faltan a cada tabla (`--missing <idioma>` los imprime como
     líneas `T(...)`); `SAVAGEWIND_MENU_LANG=<código>` (p. ej. `ja`) fuerza el idioma de los menús para probar.
     Las doce tablas están completas (es, fr, de, it, nl, ru, pt, ja, ko, zh-Hans, zh-Hant, th). ImGui 1.91.9 solo
     corta líneas en espacios: `third_party/imgui/imgui_draw.cpp` (`ImFont::CalcWordWrapPositionA`) lleva un
     parche local que también corta entre caracteres CJK, respetando la puntuación. El tailandés no separa
     palabras con espacios: sus textos llevan espacios entre frases, que es donde se cortan las líneas. Su fuente
     es Noto Sans Thai Medium (OFL, `third_party/fonts/NotoSansThai`, empaquetada como Fira Sans), y si falta, la
     del sistema (Leelawadee UI, Tahoma). ImGui no posiciona marcas combinantes con GPOS: las vocales y tonos de
     Noto Sans Thai tienen avance 0 y caen sobre la letra anterior, que se lee bien en los tamaños del menú.
     Dentro del juego, las páginas de Opciones se dibujan con la fuente tailandesa del propio juego.
   - Ajustes dentro del juego (2026-10-18): la pantalla Ajustes del propio juego (pausa > Sistema > Ajustes, u
     Opciones en el menú del título) lleva las páginas de Savage Wind y sustituye al overlay en el juego (F1, Esc y el
     botón Home ya no lo abren; `SAVAGEWIND_OVERLAY=1` lo devuelve; Esc suelta el cursor). La primera página es una
     fila por sección: Jugabilidad (las opciones del juego sin amiibo, el idioma de voz ni el modo de las gafas VR),
     Vídeo, Audio, Controles (las tres de cámara del juego y las del ratón), Teclado (botones / sticks), Mando e
     Idioma, más "Salir del juego". El juego construye la pantalla cada vez que se abre a partir de una tabla
     estática de 14 entradas (0x7001cccd98..0x7001cccdb0): `runtime/ui/game_options.mm` la apunta a una copia con sus
     entradas detrás y cambia la lista de filas de cada página; los ganchos de `tools/patches.py` le pasan las filas
     de esas entradas (valor, textos, A) y un cambio de página cierra la pantalla, que el menú que la abrió vuelve a
     abrir: el update del menú del título (pantalla 1, botones 0x69..0x6f, `FUN_7100d4c5e0`) o el estado Ajustes de
     la página Sistema de la pausa (botones 0x4d..0x53, `FUN_7100cbc918`). "Salir" (en Ajustes y en lugar de
     "Ajustes de amiibo" en el título, sin soporte de amiibo) pregunta con el diálogo de dos botones del juego
     (pantalla 0x39, tipo 7, textos propios): escritorio o pantalla de inicio de Savage Wind; la respuesta queda en
     0x7001c4b308 (botón de arriba 0, de abajo 1) y B cierra sin respuesta. Textos nuevos solo en inglés y español
     por ahora (`tools/i18n_check.py`).
   - Sin "Contenido descargable" en el menú del título (2026-10-22, issue #19): su botón de la eShop abría el applet
     de la tienda, que no existe (ILibraryAppletCreator es un stub), y el juego esperaba para siempre. Hasta que haya
     DLC, el botón (id 0x6d, panel Pa_SaveFile_04, el último del menú) sale con los medios del propio juego, como
     cuando el juego deja fuera botones (sin partida no hay Continuar): (1) el menú habilita al empezar los botones
     de una de sus listas estáticas {u32 cuenta, ids} (una por variante, en la .bss de main, llenas por un
     constructor estático); las que tienen 0x6d lo tienen al final, así que su cuenta baja en uno justo después de
     los constructores (`rt_title_menu_prepare`, llamado desde `runtime/main.c`; las listas se encuentran por su
     contenido, ids 0x69..0x6f ascendentes), y el cursor ya no llega a él; (2) lo que se ve lo decide el cuadro de la
     animación `Title_00_BtnNum`, cuyas curvas de visibilidad muestran Salir y Contenido descargable siempre juntos:
     en sus datos (en el archivo de layout cargado) las claves de visibilidad de Pa_SaveFile_04 pasan a ocultas en
     cada update del menú (`title_hide_dlc`; el manejador de la animación está en el campo `title_options_state`
     - 0x24, en 1.8.2 y 1.9.0). Era el último botón: no queda hueco.
   - setjmp / longjmp (2026-10-16): libjpeg y libpng de `nn::image` salen de sus errores con `longjmp`, que vuelve
     a la mitad de la función que llamó a `setjmp`; el código recompilado no puede retomar ahí ("no function at
     70023f1b14", al codificar la miniatura del guardado). `tools/gen.py` envuelve cada `bl` a `setjmp` (el
     símbolo o su stub PLT) en un `setjmp` del host dentro de la función C que llama (`setjmp_call`), con un
     `jmp_buf` del host por cada `jmp_buf` del juego y por hilo (`rt_setjmp_slot`). El `setjmp` de musl sigue
     corriendo y llena el del juego; `longjmp` es HLE (`hle_longjmp`, `runtime/hle/setjmp.c`): restaura x19-x30,
     sp y d8-d15 de ese `jmp_buf` y salta al del host. Solo el `sdk` los usa (10 llamadas).
   - Compilación de shaders síncrona por defecto (2026-10-16): un draw cuyo programa o pipeline todavía compilaba
     en otro hilo se descartaba. Lo que el juego dibuja una sola vez se perdía: la miniatura del guardado (dos
     blits, 1920x1080 a 854x480 a 256x144, leídos enseguida con `CopyTextureToBuffer`) salía con lo que hubiera en
     esa memoria de la GPU. Ahora el draw espera la compilación (un tirón la primera vez que aparece un efecto; el
     caché de SPIR-V y de pipelines lo evita en las siguientes sesiones). Ajuste "Asynchronous shaders"
     (`async_shaders`) para volver a no esperar; `SAVAGEWIND_SYNC_SHADERS` / `SAVAGEWIND_ASYNC_SHADERS` mandan
     sobre el ajuste. Aun en asíncrono, los draws en texturas array, 3D o cubo que no siguen la escala de salida
     esperan su pipeline (2026-10-22, `draw_must_happen` en `exec.c`): BOTW copia cada tile de terreno que carga una
     sola vez a su capa de los arrays de altura, normales y materiales, y una copia descartada dejaba el material 0
     (hierba) para siempre: bandas verdes en la arena fuera de la Ciudad Gerudo en ~3 de 4 corridas. Las esperas
     son de la carga; la línea `[vk] t=` las cuenta como `build waits N`.
   - Control de flujo estructurado en el GLSL (2026-10-23): un shader con saltos se traducía como una máquina de
     estados (`for (guard) switch (pc)`, con pilas para SSY / PBK / PCNT), y el driver tarda mucho más en compilar
     eso: en una muestra de shaders del juego, los que la usaban eran el 94 % del tiempo del driver y costaban ~2 veces
     más por KB de código Maxwell. Ahora los saltos que anidan se escriben como if / else y do / while
     (`structure_flow` en `translate.c`):
     - `@P BRA L; <a>; L:` → `if (!P) { <a> }`, y con un salto incondicional al final de `<a>`, if / else (un SYNC a
       la unión de su SSY cuenta igual);
     - un BRA hacia atrás → `do { } while (P)`, con el mismo tope de iteraciones que tenía la máquina de estados;
     - EXIT → `exited = true`, y lo que sigue, solo `if (!exited)`;
     - un salto a donde va a parar el rango que lo contiene (la unión de un if / else de fuera) cierra el rango.
     Lo demás (bucles con PBK / BRK / PCNT / CONT, BRX, salir de un do / while) sigue con la máquina de estados: 5 de
     286 shaders en una muestra de 1 de cada 50. `SAVAGEWIND_XLATE_NO_STRUCTURE=1` la usa siempre.
     Muestra de 1 de cada 50 shaders del juego (Radeon 660M, RADV, 4 hilos): compilación del driver 42,9 → 14,6 s de
     CPU, reloj 12,5 → 5,5 s. El MSL no cambia (`tools/test_translate.sh`: 0 diferencias en 20.000 programas); el GLSL
     de la prueba compila y valida igual que antes (mismos fallos, del #38), y `tools/test_vk_pipelines` crea los
     pipelines sin mensajes de la capa de validación. En el juego, con las cachés calientes y
     `SAVAGEWIND_MAX_INTERVAL=2`, las capturas de los frames deterministas (200, 400, 600, 1600 desde una partida
     nueva) son idénticas píxel a píxel a las de la máquina de estados, y las demás difieren lo mismo que dos corridas
     de la máquina de estados entre sí.
   - GLSL más barato para el driver (2026-10-23): el GLSL traducido llamaba a un ayudante del preludio por cada
     instrucción (`as_f(r3)`, `as_u(fma(...))`): una llamada a función que glslang deja en el SPIR-V y que el driver
     integra una a una (880.000 `as_f` en 1.500 shaders del juego). Ahora:
     - `uintBitsToFloat` cuando el argumento es un registro, una palabra de cbuf o un literal uint, y
       `floatBitsToUint` en los resultados float (`fresult`).
     - El preludio entra con lo que el shader llama (`glsl_with_prelude`): glslang analizaba sus 32 funciones en cada
       shader, ~2 ms.
     - El SPIR-V sale sin nombres de depuración (`strip_names`, `OpName` y compañía): un 16 % menos de fichero.
     Muestra de 1 de cada 10 shaders del juego (Radeon 660M, RADV, 6 hilos, con el control de flujo estructurado de
     #48): compilación del driver 83,8 → 28,8 s de CPU y reloj 19,8 → 10,2 s. El MSL no cambia
     (`tools/test_translate.sh` contra el commit anterior: 0 diferencias en 20.000 programas).
   - Herramientas de diagnóstico:
     - `SAVAGEWIND_THREAD_DUMP_AT=<s>,<s>`: estado, núcleo y cadena de llamadas de cada hilo; para los que
       esperan un mutex, también su palabra (el handle del dueño).
     - Audio: `SAVAGEWIND_AUDIO_LOG=1` (cada 10 s: tiempo de render, espera del mutex del renderer, cortes del
       dispositivo, pico), `SAVAGEWIND_AUDIO_WAV=<archivo>` (graba la salida), `SAVAGEWIND_AUDIO_DFC=0` (vuelve
       al HLE de `audren:u`).
     - `SAVAGEWIND_WATCH=<dir>:<largo>` (con `_DEREF`, `_TRAP`, `_STEP`, `_FOLLOW`, `_WRITES`): trampa de
       escritura por página con paso a paso (Windows), y quién escribió (`módulo+offset`), con el lr y la cadena de
       registros de marco del hilo del juego (quién llamó al `memcpy` que escribió; `swgame.dll+offset` se traduce a
       `f_<dirección>` con `nm` sobre la base 0x180000000).
     - `SAVAGEWIND_FNPROF=0:999999` con `SAVAGEWIND_FNPROF_ARGS=<dir>,...`: x0-x5 y el llamador en cada
       entrada de esas funciones; `SAVAGEWIND_FNPROF_DEREF=<reg>:<n>` vuelca lo apuntado (y muestra textos).
     - `SAVAGEWIND_NVN_LOG=1`: funciones NVN que el juego busca y enteros del dispositivo que lee.
     - `SAVAGEWIND_VK_ADDR_REPORT=<lo>-<hi>` (direcciones de GPU en hex): cada asignación y liberación de memoria de
       la GPU en ese rango (`[vkaddr]`, VK_EXT_device_address_binding_report), para saber qué había en la dirección de
       un "device fault".
     - `SAVAGEWIND_CORES=0` / `SAVAGEWIND_ONE_CORE=1`: sin emulación de núcleos / todo en el núcleo 0.
     - `SAVAGEWIND_SHOT_AT=<frame>,...` (capturas en `build/frames/`) y `SAVAGEWIND_DEBUG_FRAME=<frame>`
       (cada pase en `build/pass_NNN.png`, `_c<i>` para los demás targets; un frame del juego guarda cientos de
       imágenes y tarda minutos). `SAVAGEWIND_HIDDEN=1` corre sin ventana visible.
     - `SAVAGEWIND_SHADER_DUMP=1`: el código Maxwell de los shaders que no traducen, en `build/shaders/`.
     - `SAVAGEWIND_DEBUG_PASS_FROM=<n>`, `SAVAGEWIND_DEBUG_PASS_TO=<n>` y `SAVAGEWIND_DEBUG_MAIN_ONLY=1` (con el
       frame de depuración, F9): guarda solo esos pases y solo el primer target de cada uno. Con
       `SAVAGEWIND_DEBUG_FRAMES=<n>`, n frames seguidos (`build/pass_<frame>_NNN.png`).
     - F12: guarda una imagen del juego de cada dos en `build/frames/rec_N.png` hasta el siguiente F12, para ver
       parpadeos de un frame (el juego va más lento mientras graba).
     - `SAVAGEWIND_TRACE_TEX=<an>x<al>[,...]` o `all`: cada textura (por su memoria) guarda sus últimas escrituras
       (draws con el shader, el uniform buffer del VS y las texturas que muestrean; copias; subidas desde la memoria
       del juego). Al copiarse una a un buffer (la CPU la lee: una captura) se loguea su historia y la de sus
       fuentes, y se guardan como `build/trace_<n>_<id>.png`. `all` baja el juego a ~20 FPS.
     - `SAVAGEWIND_LOG_JPEG=1` (o `dump`, que guarda la primera imagen en `build/jpeg_in.raw`): cada
       `JpegEncoder::Encode` con su tamaño, calidad, espacio y resultado, y de dónde vienen los píxeles. BOTW baja
       la calidad de 5 en 5 hasta que la miniatura entra en 10 KB. `SAVAGEWIND_LOG_LONGJMP=1`: cada `longjmp`.
     - En el frame de depuración: `[draw]` muestra la `VkImage` de cada target y textura, los factores de blend,
       el depth range y el polygon offset; `SAVAGEWIND_DEBUG_DATA=1` vuelca también los cbufs del VS (y sus
       constantes `c1`) de los draws de pantalla completa; `SAVAGEWIND_LOG_BINDS=1`, los binds de texturas y
       uniform buffers y las actualizaciones inline; `SAVAGEWIND_LOG_OPS=1`, cada record ejecutado, los `CALL`
       y los envíos.
     - `SAVAGEWIND_DBG_FS=<fs hex>:<pc hex>:<reg>`: ese registro tras esa instrucción como salida del fragment
       shader (`pc` ffffffff: 1.0 donde el shader corre), ahora también en Vulkan. `SAVAGEWIND_SAVE_MSL=1`
       guarda el GLSL de cada shader en `build/glsl/`.
     - Nombrar un fragment shader sin su dirección (cambia en cada corrida): en `SAVAGEWIND_SKIP_FSHASH`,
       `SAVAGEWIND_DBG_FS` y `SAVAGEWIND_DEBUG_TEXDUMP` vale `w<pc>=<palabra>`, los shaders cuya instrucción en ese
       `pc` es esa palabra (los comentarios `// pc` del GLSL guardado), además del hash del log `[draw]`.
     - F8: dibuja / vuelve a saltar los shaders de `SAVAGEWIND_SKIP_FSHASH` en vivo (A/B en la misma vista). F10 es
       el overlay de rendimiento.
     - `SAVAGEWIND_DEBUG_TEXDUMP=<fs>` (con F9): el primer draw de ese shader guarda sus texturas canal por canal
       y capa por capa (`build/dtex_<hash>_<i>[_L<capa>]_<r|g|b|a>.png`, profundidad en `.pgm`), su layout de
       vértices con los primeros vértices en hexadecimal, y sus uniform buffers enteros. Con
       `SAVAGEWIND_DEBUG_TEXDUMP_ALL=1`, cada draw de ese shader guarda además sus texturas
       (`build/dtex_<hash>_d<draw>_<i>.png`). El log `[draw]` del frame muestra el sampler (filtros, wrap, LOD),
       el handle y el swizzle de cada textura, su forma (capas) y las vistas de los render targets.
       `SAVAGEWIND_DEBUG_TEXDUMP_LAYERS=<primera>-<última>` elige las capas (por defecto 0-7),
       `SAVAGEWIND_DEBUG_TEXDUMP_LEVEL=<n>` el nivel (estirado al tamaño del 0) y `SAVAGEWIND_DEBUG_TEXDUMP_DRAW=<n>` el
       draw (numerado como en `[pick]`; cambia entre corridas) en vez del primero; `SAVAGEWIND_DEBUG_TEXDUMP_NTH=<n>`, el
       n-ésimo draw de ese shader en el frame (el cielo de BOTW: 1 es el del cubemap, 2 el de la vista).
     - `SAVAGEWIND_LOG_FS_CB=<fs hash>:<k>:<primer float>:<cuántos>`: en cada frame, esos floats del uniform buffer `c[k]`
       del fragment shader (el `fcb<k>` del frame de depuración) en su primer draw; sigue un parámetro a lo largo de un
       recorrido con guion (así apareció la paleta de 60 s). En el frame de depuración, el log `[draw]` termina con
       `clamp` y cada `ReportCounter` que no es de tiempo sale como `[counter]` (tipo, destino, draw y shader).
     - `SAVAGEWIND_DEBUG_PICK=<x>,<y>[;<x>,<y>...]` (hasta 4 puntos en coordenadas de 1600x900, con F9): después
       de cada draw copia esos píxeles de su primer target; al terminar el frame, `[pick]` lista por punto los
       draws que los cambiaron (el último es el que se ve) con su shader y hash. Con `SAVAGEWIND_DEBUG_FRAME=<frame>`
       no hace falta F9. `SAVAGEWIND_DEBUG_PICK_RT=<i>` mira el target i de cada pase en vez del primero (el albedo
       del G-buffer es el 1); los pases sin ese target no cuentan.
     - `SAVAGEWIND_LOG_DRAW_INTO=<dirección hex>`: en todos los frames, cada draw en la textura de esa dirección
       (`[into]`): capa, viewport, scissor, shader y, por textura que muestrea, su objeto, si tiene imagen y un hash
       de su memoria en ese momento (así salió la copia de tiles de terreno al array de materiales).
     - Memoria que comparten la CPU y la GPU: `SAVAGEWIND_LOG_COPIES=1` (copias buffer/textura y
       `nvnTextureWriteTexels`), `SAVAGEWIND_LOG_RENDER_TEX=<an>x<al>` (pases que dibujan en texturas de ese tamaño, con
       nivel y capa), `SAVAGEWIND_DUMP_LINEAR=1` (lo que vuelve de los render targets lineales, crudo, en
       `build/linrt_*.raw`), `SAVAGEWIND_DUMP_RAW=<an>x<al>x<formato hex>` (la memoria del juego de esas texturas al
       subirlas, `build/raw_*.bin`), `SAVAGEWIND_DUMP_TEX3D=1` (texturas 3D ya desenredadas, `build/tex3d_*.raw`),
       `SAVAGEWIND_NO_LINEAR_READBACK=1` (A/B sin la copia de vuelta) y `SAVAGEWIND_LOG_CMD_COUNTS=1` (cuenta de
       comandos NVN en cada intervalo de estadísticas). Las funciones de `nvnCommandBuffer` sin implementar se
       loguean la primera vez que el juego las llama.
     - Ir a otras regiones sin jugar (sólo pruebas, nunca para el jugador; `runtime/services/test_warp.c`):
       `SAVAGEWIND_TEST_WARP=<lugar>` o `<x>,<y>,<z>[,<tipo de mapa>/<mapa>]` reescribe la posición guardada en
       `game_data.sav` mientras el juego lo lee (el archivo no cambia) y le da el planeador a Link, así que
       "Continuar" carga allí. Lugares: plateau, kakariko, stable, hateno, castle, castle_in, desert, gerudo, hebra,
       zora, goron, rito, ruta y rudania (dentro de las bestias divinas; en Rudania Link arde sin protección
       contra el fuego). `SAVAGEWIND_TEST_TIME=<hora>` fija la hora del día escribiendo la de TimeMgr en cada
       frame (al continuar, el juego no toma la hora de la partida). `SAVAGEWIND_TEST_WEATHER=<n>` (0 despejado,
       1 nublado, 2 lluvia, 3 lluvia fuerte, 4 nieve, 6 tormenta) cambia los pronósticos guardados, pero el clima al
       cargar no los sigue. `SAVAGEWIND_TEST_FLAGS=<flag>=<valor>,...` cambia otros datos (p. ej.
       `MaxHartValue=120,CurrentHart=120`, 30 corazones). Usar con una copia de la partida
       (`SAVAGEWIND_SAVE_DIR`): al guardar, el juego escribe esos valores. Continuar con
       `SAVAGEWIND_INPUT="155:A:4,187:A:4,213:A:4"`.
     - En Linux, `tools/linux/test_run.sh <carpeta de una ranura> [segundos]` hace todo eso con una copia de la
       partida en `build/test_run/` (ini propio, ventana oculta, "Continuar" con A): jugable hacia el frame 1100,
       ~30 s. Pasa las `SAVAGEWIND_*` que se le den (`SAVAGEWIND_TEST_WARP=hateno`…); `TEST_INPUT` añade botones
       del juego (formato de `SAVAGEWIND_INPUT`, `1400:PLUS:6` abre la Alforja) y `SHOTS=<n>` captura cada n frames.
       Ojo: `pad:` de `SAVAGEWIND_HOST_INPUT` usa nombres de SDL (`start`, `south`), no `PLUS`.
     - En Git Bash, `VAR=1 timeout 60 ./build/savagewind.exe` no le pasa la variable al programa: usar
       `timeout 60 env VAR=1 ./build/savagewind.exe`. Con `/c/msys64/usr/bin` primero en el PATH, `env` es el de
       MSYS2 y tampoco ve las variables exportadas desde Git Bash: pasarlas como argumentos de `env`.
     - `tools/ghidra/gq.py data <dirección> [n]` (palabras de 64 bits y a qué apuntan). Con gdb de MSYS2
       (`gdb -p <pid>`) se ve la pila nativa con los nombres `f_<dirección>` de `swgame.dll`, y la memoria del
       juego se lee en las mismas direcciones.

Se conserva todo lo genérico: el lifter AArch64 → C, el kernel HLE, IPC y servicios, el HLE de NVN
con el backend Vulkan, el traductor de shaders Maxwell, ASTC, audio, la UI de ajustes y la
compilación local con `zig cc`.

## Módulos en memoria

Las direcciones de los módulos del exefs se deciden en un solo lugar, `tools/layout.py`, igual que el
loader de Switch: uno detrás de otro en orden de carga (`main`, `subsdk0`..`subsdk9`, `sdk`; `rtld` no
se carga, su trabajo lo hace el runtime), cada imagen (text, rodata, data + bss) redondeada a 4 KiB,
desde `MAIN_BASE` (0x70_0000_0000) y sin pasar de `MODULES_END` (0x70_8000_0000: 2 GiB para módulos).
Ambos límites están en `runtime/rt.h` y en `tools/layout.py`.

- `make gen` (`gen.py all`) escribe `generated/layout.txt` y `generated/layout.c` y genera cada módulo en
  `generated/<módulo>/`. `gen.py <módulo> <nso> <layout.txt> <salida>` genera uno solo con esas
  direcciones y se niega si el NSO no es el del layout (build id).
- `layout.c` se compila dentro de la biblioteca del juego (`runtime/gamelib.h`, `GAMELIB_ABI` 3): por
  cada módulo, nombre, dirección, tamaño, build id y tabla de funciones; además la versión del juego y sus
  sitios (ver "Versiones del juego"). `runtime/loader.c` carga cada
  módulo donde dice la biblioteca y se detiene si un NSO tiene otro build id o si hay un `subsdk*` que
  la biblioteca no tiene (hay que volver a generar).
- La pantalla de setup (`runtime/gamebuild.c`) corre `gen layout` y después un generador por módulo.
- `tools/sw_probe.py` imprime la misma disposición y si el runtime la puede cargar.
- `tools/nso.py` y `runtime/loader.c` leen `DT_RELR` (relocaciones relativas empaquetadas): en 1.8.2 todas
  las `R_RELATIVE` de los tres módulos están ahí (222402 en `main`, 3424 en `subsdk0`, 15007 en `sdk`).
  Sin esto los punteros de los datos quedaban sin reubicar. `gen.py` también los toma ahora como inicios
  de función posibles, pero casi todos ya los encontraba por otras vías (eh_frame, `bl`, `adrp`+`add`):
  en `main` agregan 2.
- `tools/nso.py` y `runtime/loader.c` cuentan los símbolos también con `DT_GNU_HASH`: el `sdk` y el
  `subsdk0` de 1.8.2 no traen `DT_HASH`, y sin esto se veían con 0 símbolos: los imports de `main` no
  se resolvían y `gen.py --hle` no podía encontrar funciones del `sdk` (hoy reemplaza `longjmp`, ver
  "setjmp / longjmp").
- `tools/test_layout.py` lo prueba con NSOs sintéticos sin código del juego (un `main` de 77 MB, dos
  `subsdk`, un `sdk` de tamaño impar, `subsdk0` y `sdk` solo con `DT_GNU_HASH` y `DT_RELR`):
  `tools/venv/Scripts/python tools/test_layout.py`.

Con el exefs de 1.8.2: `main` en 0x70_0000_0000 (29.2 MiB), `subsdk0` (multimedia) en 0x70_01d3_7000
(6.4 MiB), `sdk` en 0x70_023a_3000 (12.3 MiB). El loader carga los tres ahí, con 488, 10110 y 23039
símbolos; ningún import queda sin resolver salvo los `glslc*` débiles de `main`.

## Versiones del juego

Política (2026-10-22): la 1.9.0 es la principal; el entorno de desarrollo (`extracted/`, `generated/`) y las pruebas
usan la 1.9.0. La 1.8.2 queda en `extracted-1.8.2/` y se sigue aceptando en la instalación, sin soporte.

Savage Wind arma el juego con los archivos de cada jugador, así que puede servir a más de una versión a la vez. Lo
que depende de la versión son las direcciones del `main` que tocan los parches y las que lee el runtime; el resto
(la recompilación entera, el HLE del `sdk` por nombre de símbolo, NVN) sale de los archivos mismos.

- Listas de versiones: `runtime/game_versions.h` (la pantalla de configuración) y `VERSIONS` en
  `tools/game_sites.py` (el generador), con nombre, build id del `main` (los primeros 8 bytes) y si está
  soportada. `tools/test_game_sites.py` comprueba que coincidan. Hoy: 1.8.2 (`de0b6ac4efc1dda7`, 41056 ficheros de
  romfs) y 1.9.0 (`cd57b23fa4bbad65`, 41062: los 6 de más son el tailandés, `Pack/Bootup_THth.pack` y sus fuentes).
- La configuración acepta una versión soportada. Una conocida pero apagada se rechaza con su propio mensaje
  ("La versión %s aún no es compatible": Savage Wind juega la 1.8.2 por ahora). Cualquier otra se rechaza como
  antes. Los textos que nombran versiones llevan `%s`: la lista sale de `game_versions.h` ("1.8.2", luego
  "1.8.2 / 1.9.0"), sin traducir de nuevo.
- `gen.py layout` / `all` se niega con un `main` de una versión no soportada, salvo con `--any-version`
  (`make gen GENARGS=--any-version`): para levantar una versión nueva. `--no-sites` genera sin sitios ni parches
  (los módulos inventados de `tools/test_layout.py`).
- Sitios (`SITES` en `tools/game_sites.py`): 38 lugares del `main`, cada uno con nombre, tipo y patrón.
  - Parches de `tools/patches.py` (17): la pantalla Opciones (filas, valores, textos, A, B, actualización), el
    diálogo de dos botones, el menú del título, la espera de vsync del bucle de frames y `ksys::VFR`.
  - Funciones a las que llaman esos parches (7): abrir y cerrar pantallas, el tipo de flag, el texto de un
    mensaje, redibujar una fila.
  - Datos que usa el runtime (12, `runtime/game_sites.h`): la tabla de opciones, el ScreenMgr, la respuesta del
    diálogo, el archivo de mensajes del título, "Ver.%s", el estado de pausa, el manager de eventos, el paso de los
    mandos, la dirección de retorno del bucle de frames al fijar el intervalo, PlayerInfo y world::Manager (solo
    pruebas), y dónde empiezan `.data` y termina `main`.
  - Campos de objetos del juego que la 1.9.0 movió (2, tipo `imm`: el inmediato de la instrucción): el número de
    filas de la pantalla Opciones (+8: las filas; 1.8.2 +0x3698, 1.9.0 +0x36b0) y el botón activo del menú del
    título (1.8.2 +0x363c, 1.9.0 +0x3654).
- Cada patrón son unas 8 palabras de instrucciones dentro de una misma función, con los campos que se mueven
  enmascarados: offsets de `b` / `bl` / `b.cond` / `cbz` / `tbz`, la página de un `adrp` y los 12 bits bajos que le
  suma un `add` / `ldr` / `str`. El sitio es esa instrucción (`code`), la función a la que salta (`call`), la
  dirección que forma con su `adrp` (`ref`), el puntero guardado ahí (`got`, una entrada de la GOT) o su inmediato
  (`imm`). Un sitio puede llevar una lista de patrones si una versión cambió su código: gana el primero que
  encuentra un solo lugar. Mejor aún, un patrón común: `game_sites.py common <main A> <dirección> <main B>
  <dirección> [tipo]` guarda solo los bits que las dos versiones comparten (el resto queda enmascarado) y comprueba
  que encuentre un solo lugar en cada una. Así quedaron los 9 sitios que la 1.9.0 cambió.
- `KNOWN['1.8.2']` y `KNOWN['1.9.0']` tienen la dirección de cada sitio: en esa versión el patrón tiene que encontrar exactamente
  esa (autocomprobación; otra dirección es un error), y si no encuentra ninguna o varias se usa la de la tabla con
  un aviso. Una versión sin tabla necesita que todos los patrones encuentren un solo lugar.
- `gen.py layout` resuelve los sitios y los escribe en `layout.c` (`game_version`, `game_sites[]`);
  `gen.py main` los resuelve de nuevo para los parches (`patches.table()`). El runtime los toma al cargar la
  biblioteca (`gamelib_load` llena `g_site`; falta uno: no carga). Los 38 se resuelven en ~4 s.
- Lo que los sitios no cubren, a revisar a mano en cada versión: offsets de estructuras (campos de `ksys::VFR`,
  la fila de Opciones y su entrada de 0x30 bytes, ScreenMgr +0x28 / +0x30, el evento activo en +0x1d2b8,
  PlayerInfo +0x88 / +0xa4, TimeMgr +0xb8, la respuesta del diálogo), ids de pantallas (0x34 Opciones, 0x39
  diálogo, 1 título), botones del título (0x69..0x6f, 0x6c amiibo), la vtable del diálogo (+0x4e0) y el número de
  opciones del juego (14). Cada parche además comprueba la palabra de instrucción que espera y para la generación
  si no la ve. El modo FP (`FPCR.FZ` en `MXCSR`, `runtime/simd.h`) no tiene direcciones.

Pasos para una versión nueva (así se hizo la 1.9.0):

1. Extraer su exefs y romfs (romfs con la actualización fusionada) y correr
   `tools/venv/Scripts/python tools/sw_probe.py <exefs>`: build id del `main` y layout.
2. Agregar la versión, apagada, con su build id a `runtime/game_versions.h` y a `VERSIONS`
   (`tools/test_game_sites.py` comprueba que coincidan). En `game_versions.h` va también cuántos ficheros tiene su
   romfs con la actualización fusionada (`find romfs -type f | wc -l`): la preparación lo comprueba.
3. `cd tools && ../tools/venv/Scripts/python game_sites.py find <exefs>/main`: cada sitio con su dirección o
   cuántos lugares encuentra su patrón. Para uno que no encuentra exactamente uno: buscar en el `main` nuevo la
   misma función que en 1.8.2 (la dirección de `KNOWN['1.8.2']`, por sus llamadas, cadenas o código vecino) y
   `game_sites.py common <main viejo> <dirección> <main nuevo> <dirección> <tipo>` da un patrón que encuentra
   solo esos dos (o `pattern` uno para la versión nueva sola, y el sitio lleva los dos en una lista). Un offset de
   un objeto del juego que cambió y que el runtime usa pasa a ser un sitio `imm`.
4. Copiar la tabla que imprime `find` a `KNOWN['<versión>']` y revisar los offsets y ids de la lista de arriba
   contra el código nuevo.
5. `make gen GENARGS=--any-version` y `make` en un checkout con los archivos de esa versión en `extracted/`.
   Probar oculto: título, Continuar, mundo abierto, las páginas de Opciones (con Salir) y la frecuencia de frames
   (`SAVAGEWIND_CLOCK_PROBE` a 30 / 60 / 120: el reloj del juego igual).
6. Marcarla soportada en las dos listas. `python tools/game_sites.py find` debe salir sin errores para cada
   versión, y `tools/test_game_sites.py` y `tools/test_layout.py` pasar.

Con 1.8.2 el C generado es el mismo que antes de los sitios salvo `layout.c` (gana la versión y la tabla de
sitios).

La 1.9.0 (2026-10-22, rama `game-1.9.0`; archivos del usuario en `extracted-1.9.0/`, verificados): `main` 18,8 MB
(1.8.2: 17,8), `sdk` del SDK 21.4.1, layout `main` 0x70_0000_0000-0x70_01e9_8000 (30,6 MiB), `subsdk0` y `sdk` detrás.
- `game_sites.py find` encontró 29 de los 36 sitios de entonces tal cual. Los otros 7 (la actualización de Opciones,
  abrir una pantalla, el estado de Opciones de la pausa, los textos y el B del diálogo, la actualización del menú
  del título y "Ver.%s") no: las pantallas de UI de la 1.9.0 tienen 0x18 bytes más desde antes de +0x290 (la base
  abre con `strb wzr, [x0, #0x2a8]` en vez de #0x290; Opciones: filas en +0x36b0 / +0x36b8 en vez de +0x3698 /
  +0x36a0; menú del título: +0x3654 en vez de +0x363c; diálogo: +0x33c8 en vez de +0x33b0), y esos offsets estaban
  en los patrones. Se ubicaron votando ventanas cortas de la función de 1.8.2 contra la 1.9.0 y comparando lado a
  lado, y ahora tienen patrones comunes (`game_sites.py common`). Los dos campos que lee el runtime pasaron a ser
  sitios `imm` (`opt_screen_rows`, `title_options_state`), y el parche del B del diálogo toma su offset de la
  instrucción misma.
- Los 38 encuentran un solo lugar en cada versión, y cada uno coincide con el lugar que da un voto independiente
  (ventanas de la función de 1.8.2 buscadas en la 1.9.0) o con una comprobación a mano (`opt_create_rows` lee la
  tabla de opciones +0x10; la GOT que lee la pausa apunta a `screen_mgr`; "Ver.%s" está en 0x70_015c_d092).
- Iguales en la 1.9.0, mirados uno por uno en el código: `ksys::VFR` (+0x28 .. +0x150), el bucle de frames
  (+0x228 FC, +0x234 = FC +0xc, +0x250), ScreenMgr +0x28 / +0x30, la fila de Opciones (+0x50 .. +0x68) y su
  entrada de 0x30 bytes, 14 opciones del juego, la entrada de la pantalla (+0x38, bit 1 de +0x38), pantallas 0x34
  y 0x39, botones 0x6a y 0x6c, la vtable del diálogo (+0x4e0), el evento activo (+0x1d2b8), la respuesta del
  diálogo (0xd), PlayerInfo +0x88. El único símbolo HLE (`longjmp`) sigue en el `sdk` 21.4.1.
- La RomFS 1.9.0 tiene 6 ficheros más, el tailandés: `Pack/Bootup_THth.pack`, `Font/Font_TH.sbfarc` y cuatro
  fuentes `.sbfotf`. Savage Wind todavía no ofrece el tailandés como idioma del texto.
- Pruebas (ocultas; datos en `build/run190/extracted` con `exefs` y `romfs` enlazados a `extracted-1.9.0` y su
  `romfs.bin` hecho con `--build-romfs`; copia de `save/` del usuario): biblioteca generada sin que falle ningún
  parche (125144 funciones en `main`), título con "Savage Wind 0.2", Opciones desde el título (Vídeo, Fotogramas
  por segundo), Continuar al mundo abierto (la misma posición de Link que con 1.8.2), menú de pausa > Sistema >
  Opciones > Audio con la misma repetición (0,266 s y luego 0,200 s), Salir y B (las mismas pantallas que 1.8.2), y
  a 60 FPS 30,02 frames de consola y 1,00 minuto de juego por segundo, caminar 5,5 m/s, esprintar 8,1 m/s, estamina
  ~300 por segundo (como 1.8.2: 30,02 / 1,00 / 5,7 / 8,2 / 305). Las mismas llamadas sin implementar que con 1.8.2.
  La preparación acepta la carpeta 1.9.0 (versión "1.9.0", 39008+ ficheros) y rechaza `exefs` 1.9.0 con `romfs`
  1.8.2 y al revés.

## Audio

El `sdk` de BOTW trae el AudioRenderer entero: el servidor (`nn::audio::server`: voces, mezclas, efectos,
generación de comandos) y el procesador de comandos del DSP (`nn::audio::dsp::CommandListProcessor`), código de
CPU que se recompila con el resto. Savage Wind lo usa en vez de reimplementarlo (`runtime/services/audio.c`):

- El cliente llega al servidor dentro del proceso (`CreateAudioRendererManagerByDfc`, `sdk` 0x7100224d00) cuando
  los parámetros piden el dispositivo de render CPU: `nn::audio::OpenAudioRenderer` (`sdk` 0x7100220a70) toma ese
  camino si `AudioRendererParameter` +0x2c es 1, y con +0x30 (modo de ejecución) en 1 lo abre en ejecución manual,
  sobre el work buffer del juego, sin transfer memory. Si no, abre `audren:u`.
- El HLE de `InitializeAudioRendererParameter` pide CPU + manual, así que el juego calcula su work buffer y abre el
  renderer con esos valores. El de `OpenAudioRenderer` guarda el handle y arranca dos hilos `nn::os` (el código
  del `sdk` busca su `ThreadType` en TLS +0x1f8): uno renderiza un frame de 5 ms
  (`AudioRenderSystem::ExecuteAudioRendererRendering`, en el objeto del renderer +0x70) cada vez que el stream de
  SDL baja de su objetivo, con el mutex del renderer (+0x2d0, el que toma `Update`) tomado; el otro señala el
  evento del juego, para que el render no espere el mutex del evento.
- Esos dos hilos hacen el trabajo del ADSP y corren fuera de los tokens de núcleo (`kernel_thread_off_cores`):
  el juego también usa el núcleo 3, porque el kernel informa la máscara 0xF.
- La mezcla final es un comando DeviceSink (id 0x14), que el procesador del `sdk` salta (en la consola lo toca el
  ADSP): un hook después de `CommandListProcessor::Process` lee sus buffers (int32 en la escala de int16, 6
  canales FL FR FC LFE BL BR), los baja a estéreo y los manda al stream SDL.
- Medido en el Santuario (Ryzen 5 5600X): unos 190 µs por frame (máximo 0,7 ms), espera del mutex casi siempre
  por debajo de 2 ms, 4 cortes del dispositivo en 150 s con 25 ms en cola, todos mientras carga la escena (el
  `Update` del juego retuvo el mutex hasta 30 ms). Con los hilos dentro de los tokens de
  núcleo y el evento señalado desde el mismo hilo eran 1-2 ms por frame, picos de 50-170 ms y decenas de cortes
  por segundo.
- La idea viene de SuperMarioWonderRecomp (MIT, BelmanteGu), que corre así el renderer del SDK 16 de Wonder; se
  usó su PROGRESS.md como referencia, sin copiar código.
- `SAVAGEWIND_AUDIO_DFC=0` vuelve al mezclador HLE de `audren:u` (heredado de LASRecomp), que sigue en el archivo.
  Sin probar: el reinicio en el lugar (vuelta a la pantalla de inicio) con el renderer abierto, y
  `CloseAudioRenderer`, que el juego no llamó en las pruebas.

## Sondeo del exefs

```
tools/venv/Scripts/python tools/sw_probe.py extracted/exefs --json build/probe.json
```

(En Linux: `tools/venv/bin/python`.) Imprime las direcciones que `tools/layout.py` da a los módulos y
si el runtime puede cargarlos ahí. Para cada módulo imprime:

- tamaños de segmentos y build id,
- funciones encontradas y jump tables,
- instrucciones que el lifter no traduce todavía, agrupadas por mnemónico con un ejemplo,
- tamaño estimado del C que generaría `make gen`,
- las funciones NVN que el juego nombra contra las que implementa `runtime/nvn` (faltantes y stubs).

El entorno Python se crea con:

```
python -m venv tools/venv
tools/venv/Scripts/python -m pip install capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
```

## Toolchain

Para compilar el runtime en Windows hace falta MSYS2 UCRT64 (clang, make, SDL3, FFmpeg, Vulkan,
shaderc), como describe `docs/BUILD-windows.md`. En Linux, los paquetes de `docs/linux.md`.
Comprobación rápida desde una shell UCRT64: `make runtime-objs` (compila el runtime sin enlazar).

## Primera generación y compilación completa (1.8.2)

Medido el 2026-10-14 con el exefs 1.8.2 (`main` build `de0b6ac4efc1dda7`, `subsdk0` `3308c1d216f78839`,
`sdk` `5cf080e800530a7f`) en un Ryzen 5 5600X (6 núcleos, 12 hilos) con 32 GB de RAM, Windows 11,
MSYS2 UCRT64 clang 23.1.3 y Python 3.14 en `tools/venv`. Usa el generador con el layout de módulos,
`DT_GNU_HASH` y `DT_RELR`. La biblioteca del juego (`build/swgame.dll`) compila y enlaza sin errores ni
warnings: nada del generador ni del Makefile la bloqueó. El juego no se corrió con ella (el arranque está
en "Pendiente").

Generación (`make gen`, es decir `gen.py all`: un módulo detrás de otro; `--simd c`, el default en x86-64):

| | `main` | `subsdk0` | `sdk` | total |
|---|---:|---:|---:|---:|
| funciones | 131.322 | 17.790 | 46.563 | 195.675 |
| archivos `.c` (con la tabla) | 384 | 98 | 125 | 607 + `layout.c` |
| C generado | 375,7 MB | 60,2 MB | 115,9 MB | 551,8 MB |
| tiempo | 42 s | 6 s | 13 s | 62 s |

La memoria pico de Python es la de `main`: 1,9 GB. `sw_probe.py` había estimado 449 MB para `main` +
`sdk`; el real es 491,5 MB, un 9 % más. La función más grande ocupa 1,68 MB de C. 114 archivos de `main`,
38 de `subsdk0` y 31 de `sdk` tienen una sola función (`--big-fn`: desde 100 KB de C, cada función va en
su propio archivo).

`DT_RELR` trae 222.402 relocaciones relativas en `main` (198.097 apuntan a código), pero casi todas a
funciones que `gen.py` ya encontraba por `.eh_frame`, `bl` o `adrp` + `add`: suma 2 inicios de función
en `main`, 17 en `subsdk0` y 8 en `sdk`. `runtime/hle/hle_symbols.txt` solo lista `longjmp` (del `sdk`).

Compilación (`make -j12`, con `build/savagewind.exe` ya hecho, que tarda unos 10 s):

- 19 min 1 s (1141 s) para los 608 archivos más el enlace. El enlace (lld) tarda 0,5 s y usa ~0,3 GB.
- Memoria pico: 2,9 GB sumando los 12 clang que corren a la vez; el clang más grande llegó a 731 MB.
- Objetos: 250,6 MB (`main` 188,3 MB, `subsdk0` 20,0 MB, `sdk` 42,3 MB).
- `build/swgame.dll`: 198,8 MB (`.text` 179,2 MB, `.rdata` 10,8 MB, `.pdata` 2,3 MB). Exporta solo
  `swgame_tables` e importa de `savagewind.exe` y del CRT.
- Una primera pasada con las direcciones fijas de antes (`main` y `sdk`, sin `subsdk0`) tardó casi lo
  mismo: 1171 s para 509 archivos y una DLL de 182,3 MB.

El tiempo de compilación no depende del volumen de C sino de unas pocas funciones. Cuando una función
tiene un `br` por tabla (resuelta o no), `emit_function` le pone una etiqueta a cada instrucción y un
`switch` en `L_dispatch` con un `case` por instrucción, por si la tabla emulada no cubrió algún caso.
Con 8.000 a 12.000 casos, clang -O1 se vuelve muy lento:

- Compilando 6 a la vez, `main_104.c` (una función de 1,08 MB de C, 8.089 etiquetas) tarda 553 s (9 min).
  `main_248.c`, más grande (1,7 MB) pero con 2 etiquetas, tarda 31 s.
- De los 145 archivos de una sola función de `main` y `sdk` (4.099 s de CPU, 6 a la vez), los 66 que
  tienen ese `switch` suman 3.972 s (97 %), y los 20 más lentos, 3.238 s.
- Compilados solos: un archivo normal de 1,5 MB con 535 funciones tarda 4,9 s; la tabla de `main`
  (8 MB), 2,3 s.

Bajar ese `switch` (por ejemplo, a solo las instrucciones que pueden ser destino, o despachando por
`rt_dispatch`) es lo que más acortaría la compilación, pero cambia lo que hace el código generado cuando
una tabla no se resolvió bien. (Los tiempos varían mucho con la carga: `main_248.c` tarda 10,5 s sola y
31 s con otros 5 compilando.)

Después de integrar `module-layout`, `gamelib-build` y `jump-tables` en `main` (misma máquina,
2026-10-14): `make gen` tarda 50 s (`main` 122.961 funciones en 428 archivos, `subsdk0` 13.934 en 143,
`sdk` 40.461 en 205) y `make -j12` 969 s (16 min 9 s) sin errores, con un `build/swgame.dll` de
211,7 MB. Una corrida con `savagewind.exe extracted --play` reubica los tres módulos, corre los
constructores del `sdk` y se detiene donde dice "Pendiente" 2: mensajes TIPC 16 y 17 a `sm:` y
`svcBreak(0)` en 0x700251863c.

Arreglos en `tools/sw_probe.py` de esta misma tanda:

- Las importaciones se agrupan por namespace (`nn::os` 85, `nn::audio` 74, `nn::util` 34, ...). Antes,
  402 de las 465 de `main` caían en un grupo vacío porque no se saltaba el prefijo `_ZN`.
- Las funciones NVN que enlaza `runtime/hle` (`nvnBootstrapLoader`, `nvnDeviceGetProcAddress`) cuentan
  como implementadas. Con 1.8.2: el juego nombra 569, el runtime implementa 515 (86 vacías) y faltan 54.
