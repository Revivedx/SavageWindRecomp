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
local: la importación de LASRecomp trae capturas de *Link's Awakening* y su randomizer. El `main` remoto es la
rama local `public`, un commit por publicación con el árbol de `main` y el email noreply de GitHub:

```
export GIT_AUTHOR_NAME=Revivedx GIT_AUTHOR_EMAIL=82332536+Revivedx@users.noreply.github.com
export GIT_COMMITTER_NAME=$GIT_AUTHOR_NAME GIT_COMMITTER_EMAIL=$GIT_AUTHOR_EMAIL
git branch -f public $(git commit-tree "main^{tree}" -p public -m "<qué cambia>")
git -c credential.helper= -c "credential.helper=!gh auth git-credential" push origin public:main
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

## Estructura de entrada esperada

```
savage_wind/extracted/
  exefs/   main, main.npdm, rtld, sdk (y subsdk* si el juego los trae)
  romfs/   contenido completo de la romfs (base + actualización ya fusionadas)
```

La versión objetivo es la 1.8.2 (versión de título v1048576), la última. El build id del módulo `main`
identifica la versión exacta; `tools/sw_probe.py` lo imprime.

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
     en la ventana lo vuelve a retener. Ajustes: `mouse_camera`, `mouse_sensitivity`, `mouse_invert_y`.
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
     - Vsync: la cadena de presentación sigue en MAILBOX, sin tearing; cada refresco muestra la imagen más nueva.
       Una frecuencia que divide la del monitor (60 en 60/120/240 Hz) es la más fluida; una que no (60 o 90 en
       144 Hz) alterna 2 y 3 refrescos por imagen; por encima de la del monitor las imágenes de más no se ven. La
       descripción de la opción muestra los Hz de la pantalla (`winsize_refresh_rate`).
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
     Además:
     - Copias de constant buffers: se comparten en toda la lista (antes, solo dentro de cada trozo de ~300 draws que
       va a la GPU) y `UpdateUniformBuffer` (~450 por frame en el mundo, sobre solo ~40 rangos pequeños) invalida solo
       las copias que tocan su rango (antes, todas). Lo que más se copia sigue igual: un buffer de 64 KB que el
       shader indexa con un registro (se liga entero) y que `UpdateUniformBuffer` cambia 16 bytes entre draws
       (~220 copias, ~14 MB por frame); cuesta ~6 % del hilo de la cola.
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
     Lo que sigue igual: `SAMPLES_PASSED` (tipo 1 de `ReportCounter`, la consulta del sol) devuelve siempre 0
     muestras, y `ResetCounter`, `ReportValue`, `SetRenderEnableConditional` y `SetRasterizerDiscard` no hacen nada.
     La paleta de entorno (`fcb4` del shader de la máscara de sombras) pasa de la de interior a la de exterior en 60 s
     (1/1800 por frame) al salir del santuario y al cargar una partida afuera, y no vuelve a la de interior al
     regresar: no se sabe aún si es el comportamiento del juego.
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
   - Menús traducidos (2026-10-16): la pantalla de inicio y el menú del juego siguen al idioma de texto (Auto: el
     del sistema). `tr("texto en inglés")` (`runtime/ui/i18n.mm`) busca el texto en la tabla del idioma,
     `runtime/ui/i18n/<idioma>.inc` (`T("English", "...")`; `""` deja el inglés). Los widgets de
     `option_rows.mm` y los helpers de `menu.mm` / `launcher.mm` traducen etiquetas, valores, descripciones y
     títulos ellos mismos; los IDs de ImGui siguen en inglés. Mayúsculas en UTF-8 (`tr_upper`: latín, griego,
     cirílico). Ruso, japonés, coreano y chino suman una fuente del sistema con solo los glifos que usan sus
     textos (`ui_merge_fallback_font`), y cada contexto rehace sus fuentes al cambiar el idioma.
     `tools/i18n_check.py` lista los textos que le faltan a cada tabla (`--missing <idioma>` los imprime como
     líneas `T(...)`); `SAVAGEWIND_MENU_LANG=<código>` (p. ej. `ja`) fuerza el idioma de los menús para probar.
     Las once tablas están completas (es, fr, de, it, nl, ru, pt, ja, ko, zh-Hans, zh-Hant). ImGui 1.91.9 solo
     corta líneas en espacios: `third_party/imgui/imgui_draw.cpp` (`ImFont::CalcWordWrapPositionA`) lleva un
     parche local que también corta entre caracteres CJK, respetando la puntuación.
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
     sobre el ajuste.
   - Pipelines por partes (2026-10-23): un draw con una combinación nueva de shaders y estado fijo creaba un pipeline
     entero y volvía a compilar los dos shaders aunque otro pipeline ya los tuviera (25,9 ms por pipeline en una
     Radeon 660M desde una partida nueva). Con `VK_EXT_graphics_pipeline_library` y fast linking, el pipeline se enlaza
     de cuatro partes que se guardan por contenido (`exec.c`): entrada de vértices, el vertex shader con su estado de
     rasterización, el fragment shader y la salida (formatos, blend, muestras). Las partes de los shaders, que son lo
     que cuesta compilar, dependen solo de su shader y de su descriptor set (layouts con sets independientes: el 0 del
     vertex shader, el 1 del fragment shader): un pipeline de shaders ya vistos solo se enlaza. Con
     `VK_EXT_extended_dynamic_state3` el depth clamp y el polygon mode son dinámicos, así que la parte del vertex
     shader no depende de ellos. El draw espera al enlace rápido; un hilo de prioridad baja enlaza después las mismas
     partes con optimización (LTO) y los draws pasan a ese pipeline. El precalentamiento de la lista de pipelines usa
     el mismo camino. `SAVAGEWIND_VK_NO_GPL=1` vuelve a los pipelines enteros (lo que tienen los dispositivos sin las
     extensiones); `SAVAGEWIND_VK_NO_LTO=1` deja los enlaces rápidos. La línea de `SAVAGEWIND_VK_PROF` cuenta las
     partes y los enlaces. Desde una partida nueva (150 s, caché de Mesa apagada): 25,9 → 15,1 ms por pipeline y
     16,2 → 12,6 s de compilación dentro de frames lentos. La capa de validación no da mensajes nuevos.
   - Herramientas de diagnóstico:
     - `SAVAGEWIND_THREAD_DUMP_AT=<s>,<s>`: estado, núcleo y cadena de llamadas de cada hilo; para los que
       esperan un mutex, también su palabra (el handle del dueño).
     - Audio: `SAVAGEWIND_AUDIO_LOG=1` (cada 10 s: tiempo de render, espera del mutex del renderer, cortes del
       dispositivo, pico), `SAVAGEWIND_AUDIO_WAV=<archivo>` (graba la salida), `SAVAGEWIND_AUDIO_DFC=0` (vuelve
       al HLE de `audren:u`).
     - `SAVAGEWIND_WATCH=<dir>:<largo>` (con `_DEREF`, `_TRAP`, `_STEP`, `_FOLLOW`, `_WRITES`): trampa de
       escritura por página con paso a paso (Windows), y quién escribió (`módulo+offset`).
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
       draws que los cambiaron (el último es el que se ve) con su shader y hash.
     - Memoria que comparten la CPU y la GPU: `SAVAGEWIND_LOG_COPIES=1` (copias buffer/textura y
       `nvnTextureWriteTexels`), `SAVAGEWIND_LOG_RENDER_TEX=<an>x<al>` (pases que dibujan en texturas de ese tamaño, con
       nivel y capa), `SAVAGEWIND_DUMP_LINEAR=1` (lo que vuelve de los render targets lineales, crudo, en
       `build/linrt_*.raw`), `SAVAGEWIND_DUMP_RAW=<an>x<al>x<formato hex>` (la memoria del juego de esas texturas al
       subirlas, `build/raw_*.bin`), `SAVAGEWIND_DUMP_TEX3D=1` (texturas 3D ya desenredadas, `build/tex3d_*.raw`),
       `SAVAGEWIND_NO_LINEAR_READBACK=1` (A/B sin la copia de vuelta) y `SAVAGEWIND_LOG_CMD_COUNTS=1` (cuenta de
       comandos NVN en cada intervalo de estadísticas). Las funciones de `nvnCommandBuffer` sin implementar se
       loguean la primera vez que el juego las llama.
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
- `layout.c` se compila dentro de la biblioteca del juego (`runtime/gamelib.h`, `GAMELIB_ABI` 2): por
  cada módulo, nombre, dirección, tamaño, build id y tabla de funciones. `runtime/loader.c` carga cada
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
