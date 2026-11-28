# Ray tracing y path tracing en Savage Wind: viabilidad y plan (2026-11-24; fases 0 y 1 y primeros cortes de las fases 2 y 3 el 2026-11-26)

Documento de diseño. De RT existen la fase 1 casi entera y un primer corte de la fase 2, en una rama local (ver sus secciones). El plan sale de leer el renderer actual
(`runtime/nvn/`, `docs/SAVAGE-WIND.md`, `docs/motion-vectors-spike.md`). La geometría de la escena está **medida**
(sección "Fase 0: medidas"); los tiempos de GPU y los bytes por triángulo de un BLAS siguen siendo **estimaciones**
hasta que la fase 1 construya estructuras de verdad.

## Veredicto

| Qué | ¿Viable? | Tamaño | Nota |
|---|---|---|---|
| Trazado en espacio de pantalla (GI, reflejos, sombras de contacto) sobre el G-buffer | Sí, riesgo bajo | Pequeño | Sin hardware RT, sirve en cualquier GPU. No es "RT" de verdad, pero da buena parte del cambio visual. |
| RT híbrido por hardware: sombras del sol, oclusión ambiental, reflejos, GI de un rebote | Sí, riesgo medio-alto | Grande (el mayor trabajo gráfico del proyecto hasta ahora) | El problema no es trazar rayos: es darle a la GPU la escena en espacio de mundo y saber el material de lo que el rayo toca. |
| Path tracing en tiempo real que sustituya la luz del juego | Parcial | Muy grande, después del híbrido | Posible como modo híbrido experimental en GPU altas (visibilidad primaria del G-buffer, rebotes trazados). No será un path tracer "de referencia": falta geometría fuera de cámara y los materiales de los impactos son aproximados. |
| Path tracing acumulado con la cámara quieta (modo foto) | Sí, una vez hecho el híbrido | Mediano sobre el híbrido | Es la forma honesta de ofrecer "path tracing": sin denoiser, converge en 1-2 s. |

Recomendación: hacerlo por fases, cada una entregable por sí sola y detrás de una opción apagada por defecto
(Video > Ray tracing). La fase 1 (geometría en espacio de mundo + estructura de aceleración + vista de depuración) es
la que decide si lo demás sale: si ahí la escena trazada no coincide con la rasterizada, se para sin haber tocado nada
que vean los jugadores.

## Fase 0: medidas (2026-11-26)

Instrumentación desechable en la rama local `rt-phase0` (`runtime/nvn/vulkan/rt_spike.c` y un gancho en `exec.c`
`draw_`, `SAVAGEWIND_RT_SPIKE=1`): cada 240 frames registra dos frames seguidos de los draws de escena (profundidad
`RS_SCENE` con test de profundidad) y de mapas de sombra (`RS_SHADOW`), y los compara. Corridas ocultas con guion
(Continue, quieto, andar, girar la cámara), runtime de `main` d2b4c0c6, juego 1.9.0, 1600x900 interno, 60 fps,
RTX 3080 Ti, en Meseta, Kakariko y Bosque Kolog. Solo cuenta geometría: no mide tiempos.

**Tamaño de la escena** (frame quieto, tras cargar):

| | Meseta | Kakariko | Bosque Kolog |
|---|---|---|---|
| G-buffer: draws / triángulos por frame | 2.167 / 0,54 M | 1.498 / 0,49 M | 2.118 / 0,77 M |
| G-buffer: mallas distintas / sus triángulos | 952 / 0,23 M | 727 / 0,25 M | 580 / 0,24 M |
| Pase forward (efectos, con blending) | 85 / 0,007 M | 121 / 0,010 M | 44 / 0,016 M |
| Sombras: draws / triángulos por frame | 947 / 0,38 M | 2.020 / 0,94 M | 903 / 0,55 M |
| Sombras: mallas distintas / sus triángulos | 220 / 0,11 M | 520 / 0,27 M | 181 / 0,15 M |
| Sombras: draws / triángulos que la escena no dibuja | 100 / 0,017 M | 559 / 0,22 M | 27 / 0,030 M |
| Con alfa-test (discard) en el G-buffer, triángulos | 31 % | 19 % | 48 % |
| Con huesos (cbuf >= 2 KB): draws / triángulos | 16 / 0,010 M | 24 / 0,015 M | 30 / 0,014 M |
| Mallas vistas tras ~1 min andando / triángulos | 6.027 / 1,24 M | (sin moverse) 1.890 / 0,63 M | 5.577 / 1,87 M |

Todos los draws son listas de triángulos. Casi nada va instanciado en el G-buffer (1-4 draws).

**Lo que dicen:**

1. **La escena es pequeña.** Medio millón de triángulos por frame, un cuarto de millón en mallas distintas. La
   estimación previa ("varios millones") era pesimista: los BLAS de lo visible son del orden de 20-30 MB, y todo lo
   visto en un minuto andando sin tirar nada, 100-200 MB (a 60-100 bytes por triángulo, cifra aún no medida).
2. **Los personajes no son problema.** 16-30 draws y 10-15 mil triángulos con huesos: refit de todos cada frame.
3. **La huella de constantes no sirve para decidir qué es estático.** Parado, solo el 39-67 % de los triángulos del
   G-buffer repite exactamente las constantes del frame anterior (sin contar el bloque de cámara); el resto cambia
   algo cada frame (viento en árboles y hierba: 0,07-0,14 M de triángulos leen además una textura que la GPU dibuja;
   0,06-0,27 M cambian constantes sin más, el máximo en el Bosque Kolog). Confirma la propuesta: decidir por el
   resultado (posiciones de mundo de muestra), no por las entradas. El peor caso, tratarlo todo como deformable,
   es un refit de ~0,3 M triángulos por frame.
4. **Los pases de sombra sí traen lo que la cámara no ve.** En Kakariko dibujan más triángulos que la escena, y 559
   draws (0,22 M triángulos) son rangos de índices que ningún pase de escena dibuja ese frame. En campo abierto
   aportan poco (0,02-0,03 M). Sus vertex shaders leen menos streams que los de escena, así que una malla de sombra
   y la misma de escena se emparejan por buffer de índices y rango, no por streams.
5. **El alfa-test pesa.** Entre un quinto y la mitad de los triángulos del G-buffer descartan píxeles (follaje). O van
   con any-hit (caro), o se quedan con las cascadas. Decisión para la fase 2, con medida de coste.

**G-buffer y puntos de inyección** (formatos del registro; shaders leídos con `SAVAGEWIND_SAVE_MSL=1`):

- Targets a 1600x900: 0 RGBA8 (bits de material; el terreno lo lee y escribe), 1 y 2 RGBA8 sRGB (albedo en el 1),
  3 y 4 RGBA8, 5 R11G11B10F (el buffer de luz), profundidad 0x34; un segundo subpase añade un target 7 R32F.
- Normales: RGBA8 leída como `n * 2 - 1`, sin codificación especial. La profundidad se lineariza con dos constantes
  del bloque de cámara (`cb3[16].x`, `cb3[14].x`) y la posición se reconstruye con rayos de vista interpolados.
- **Sombra del sol diferida:** tres fragment shaders de pantalla completa (`8c42c9bc921ed716`, `6e5c0c0e4e68df7d`,
  `2a77d31aaea378c2`) leen las cascadas (896x896x3 D16, `sampler2DArrayShadow`, 5-6 lecturas PCF cada uno) más los
  targets 3 y 4 (y 1 y 2), y escriben el target 5. Sustituir esas lecturas por una máscara trazada es un cambio
  acotado en el traductor para tres shaders.
- **Sombra en forward:** agua, terreno y hierba leen la copia R16 448x448x3 (`aaed7dc1f198ccc2`, `ff81794d26ec2784`,
  `3e0b11fb8ce124b6`, a 800x450) y `0f2db3c5362debb1` (200x112 y 100x56: luz volumétrica). Se quedan con las cascadas
  al principio.
- **Composición de luz:** nueve shaders de pantalla completa (`f607b5ab4c2f0720` y otros ocho) leen los targets 0-4;
  el que se leyó muestrea además un `samplerCube` (la probe) y un `sampler2DArray`: ahí entra el ambiente trazado.

**Queda sin medir** (pasa a la fase 1): tiempo real de build y refit, bytes por triángulo, dónde está la rugosidad,
dirección y color del sol y lista de luces locales en las constantes, y la escena en un santuario.

## Fase 1, primera parte: captura y estructura por frame (2026-11-26)

Rama local `rt-phase0`, detrás de `SAVAGEWIND_RT=1` (sin ella nada cambia: ni extensiones ni programas nuevos).
Hecho a lo bruto a propósito, para contestar antes de escribir la caché si la captura es correcta y cuánto cuesta
reconstruirlo todo:

- **Extensiones** (`device.c`): `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
  `VK_KHR_deferred_host_operations` y `bufferDeviceAddress`, solo si el dispositivo las tiene.
- **Captura** (`maxwell/translate.c`, `ShaderKey.rt_cap`): no hizo falta una variante compute. El propio vertex shader
  del draw, en una variante, escribe además la posición de cada vértice en un buffer, por índice de vértice; el draw
  se pinta igual. Así el pipeline sigue leyendo los atributos y el buffer de índices del juego sirve tal cual.
- **Posición sin z de clip.** La primera versión invertía la proyección completa y solo acertaba lo cercano: lejos,
  `z / w` es 1 menos casi nada y no quedan dígitos. La que funciona usa `w` de clip (la profundidad de vista, exacta)
  y deja `z` fuera. Posiciones relativas a la cámara de la escena, en ejes de mundo.
- **Estructura** (`vulkan/raytrace.c`): cada frame, un BLAS con todas las listas de triángulos del pase G-buffer
  (una geometría por draw, las de alfa-test marcadas no opacas) y un TLAS de una instancia.
- **Comprobación:** un pase compute traza dos rayos por píxel desde la cámara (todo, y solo lo opaco) y compara el
  punto de impacto con el que da la profundidad del juego. `SAVAGEWIND_RT_SHOT=<n>` guarda la imagen cada n frames.

**Medidas** (corridas ocultas con guion, runtime de `main` d2b4c0c6 más la rama, 1.9.0, 1280x720 interno, 60 fps,
RTX 3080 Ti; tiempos de GPU por timestamps alrededor de cada paso):

| | Meseta | Kakariko | Bosque Kolog | Vah Ruta (interior) |
|---|---|---|---|---|
| Mallas / triángulos capturados | 1.964 / 0,50 M | 1.284 / 0,45 M | 1.165 / 0,48 M | 626 / 0,24 M |
| BLAS, construcción completa (fast build) | 3,4-3,9 ms | 3,1 ms | 3,2-3,9 ms | 1,7 ms |
| TLAS | 0,03 ms | 0,03 ms | 0,03 ms | 0,03 ms |
| Dos rayos primarios por píxel | 3,6 ms | 2,6 ms | 2,5 ms | 2,0 ms |
| Tamaño del BLAS (sin compactar) | 26 MB | 23 MB | 25 MB | 12,5 MB |
| Píxeles a menos del 0,5 % de la profundidad | 73-84 % | 84,5 % | 83-94 % | 10-82 % |
| ... entre 0,5 % y 2 % | 0,6-2 % | 5,1 % | 2,5-6,5 % | 0,1 % |
| ... que casan al ignorar o atravesar alfa-test | 14-26 % | 10,3 % | 2-11 % | 0,5 % |
| **Sin explicar** (sin impacto, más cerca o más lejos) | 0,00-0,05 % | 0,01-0,02 % | 0,00-0,02 % | **17-90 %** |

54 bytes por triángulo, medido (la estimación era 60-100). Parado y andando dan lo mismo en mundo abierto.

**Lo que dicen:**

1. **La captura es correcta en mundo abierto**: menos del 0,05 % de los píxeles sin explicar en los tres sitios, en
   parado y en movimiento. El criterio de paso de la fase 1 se cumple ahí.
2. **Reconstruirlo todo cada frame cuesta 3-4 ms** de GPU en una 3080 Ti. Es jugable para probar, pero es un 20 % de
   un frame a 60 fps: la caché de BLAS estáticos y el refit siguen mereciendo la pena, y son lo siguiente.
3. **El alfa-test es el 10-26 % de los píxeles** en exteriores: sin any-hit con la textura, un rayo ve las tarjetas
   de follaje enteras. Confirma que es la decisión de coste de la fase 2.
4. **Vah Ruta falla.** (Diagnóstico inicial, erróneo: "falta el agua". La causa real y el arreglo están en la
   segunda parte: dos mundos con cámaras distintas en el mismo pase.)

**Hallazgos de implementación:**

- En los píxeles de cielo ningún rayo toca nada: el cielo no entra en la estructura.
- Los targets de la escena guardan la imagen al revés de lo que dice el `flip_y` de sus programas; medido, no
  explicado.
- El disparo de la construcción es el primer draw de pantalla completa a un target de escena de media resolución
  (`2be2b4c654bc45c0`, la reducción de profundidad), justo antes de los shaders de sombra del sol: el TLAS llega a
  tiempo para la fase 2.
- Cambiar `translate.c` y `shader.h` cambia el identificador del traductor: la caché de shaders se rehace una vez.
  El tamaño de `ShaderKey` no cambia (el campo nuevo va en relleno).

**Falta de la fase 1:** caché de BLAS (estático por posiciones de muestra, rígido por transformación, deformable con
refit), hilo propio, liberación diferida y presupuesto de VRAM (ahora son reservas fijas: 64 MB de vértices y 3 x 48 MB
de índices), claves nuevas en `precompile.c`, captura de los pases de sombra (`SAVAGEWIND_RT_SHADOW=1`, escrita y sin
probar), el agua de Vah Ruta, un santuario de verdad, medir el coste en fps con y sin, y la vista en pantalla (hoy es
un PNG). Con TAA activo la variante de captura convive con la de vectores (comparten push constants) pero no se ha
probado.

## Fase 1, segunda parte: estructuras por malla con refit (2026-11-26)

Misma rama, mismo equipo y resolución que la primera parte. `SAVAGEWIND_RT_MODE` elige: 1 (por defecto) lo de esta
sección, 2 lo mismo reconstruyendo cada malla cada frame, 0 la primera versión (un solo BLAS).

**Correcciones a la primera parte:**

- **Vah Ruta no fallaba por el agua.** Ningún draw quedaba fuera. En el interior de una Bestia Divina el juego dibuja
  en el mismo pase dos mundos con cámaras distintas (la mazmorra y el exterior, a 3,7 km en sus coordenadas) que
  representan el mismo ojo. Se unifican por espacio de vista: toda cámara en perspectiva del pase G-buffer se coloca
  con la rotación y la posición de la primera. Con eso Vah Ruta casa como el resto.
- **Culling de caras.** Un follaje grande visto desde atrás daba un frame con el 62 % de los rayos chocando antes de
  tiempo: el rasterizador descarta esas caras y un rayo no. Cada instancia lleva ahora el culling de su draw, para
  los rayos que lo pidan (los de la comprobación; los de sombra decidirán en la fase 2).

**Diseño.** Un BLAS por malla, que se conserva entre frames y se **reajusta** (refit) a las posiciones del frame, y
un TLAS con una instancia por malla. "Malla" es el rango de índices y los streams de un draw, más qué número hace
entre los draws iguales del frame. No se casa cada draw con "el mismo objeto" del frame anterior ni se decide qué es
estático: un refit solo exige los mismos triángulos, y la misma malla en otro sitio ajusta igual de bien. Evita los
dos problemas que el plan daba por necesarios (identidad entre frames y detección de estático por posiciones de
muestra) a cambio de reajustar todo cada frame. Los índices se copian una vez por malla, no por frame; una malla que
nadie pide en 240 frames se libera y su hueco se reutiliza.

**Medidas** (Kakariko quieto, 0,45 M de triángulos, 1.300 mallas):

| Modo | Estructuras por frame | Dos rayos primarios por píxel |
|---|---|---|
| 0: un BLAS reconstruido | 3,21 + 0,03 ms | 2,6 ms |
| 2: BLAS por malla, reconstruidos | 2,63 + 0,23 ms | 4,6 ms |
| 1: BLAS por malla, refit | **0,41 + 0,23 ms** | 5,0 ms |

Modo 1 en los cuatro sitios (guion con movimiento, ~140 muestras por sitio, una cada 30 frames):

| | Meseta | Kakariko | Bosque Kolog | Vah Ruta |
|---|---|---|---|---|
| Refit de todo + TLAS | 0,12 + 0,22 ms | 0,41 + 0,23 ms | 0,43 + 0,22 ms | 0,22 + 0,19 ms |
| Dos rayos primarios por píxel | 2,6 ms | 4,7 ms | 3,5 ms | 3,5 ms |
| Píxeles sin explicar, media | 0,08 % | 0,01 % | 0,00 % | 0,02 % |
| ... peor muestra | 6,8 % | 0,04 % | 0,01 % | 1,7 % |
| Mallas guardadas / MB de estructuras (máximo visto) | 2.762 / 40 | 1.343 / 32 | 8.324 / 173 | 9.147 / 205 |

**Lo que dicen:**

1. **El refit baja las estructuras de 3,2 ms a 0,6 ms por frame**, sin caché de estáticos y sin lógica de identidad.
2. **Trazar sale casi el doble de caro** (5,0 contra 2,6 ms en Kakariko): un TLAS de más de mil instancias que se
   solapan, y BLAS con permiso de refit. Con dos rayos por píxel los dos modos empatan en total; cuál gana depende de
   cuántos rayos lance la fase 2. Sin optimizar: juntar mallas pequeñas vecinas en un BLAS es lo primero a probar.
3. **La memoria sube al moverse**: al girar la cámara cambia qué número hace cada draw y se crean mallas nuevas
   (hasta 9.000 guardadas, 205 MB). Se liberan a los 4 s; hace falta un presupuesto de VRAM de verdad.
4. **Quedan frames sueltos sin casar** (2 de 139 muestras en Meseta y en Vah Ruta, hasta el 6,8 %). Sin investigar;
   coinciden con cargas y cortes del guion, no comprobado.

**Falta de la fase 1:** presupuesto de VRAM (`vram.c`) y arena de tamaño fijo (384 MB reservados), los frames sueltos,
hilo propio (medir antes el coste de CPU en el hilo de traducción: no medido), claves en `precompile.c`, captura de
pases de sombra (sin probar), un santuario de verdad, fps con y sin, TAA activo, y la vista en pantalla.

## Fase 1, cierre, y fase 2, primer corte: sombras del sol trazadas (2026-11-26)

**Lo que faltaba de la fase 1 y se cerró:**

- **Frames sueltos sin casar: tres causas, las tres de captura.** (1) Draws del pase G-buffer cuyo vertex shader no lee
  el bloque de cámara: se colocan con la cámara de la escena (escriben en el mismo espacio de clip). (2) Draws
  instanciados (hojas, partículas con profundidad): cada instancia escribe sus vértices a continuación de la anterior
  y la malla lleva todas. (3) Quads de 6 índices (suelo y agua planos) que una heurística heredada tomaba por draws de
  pantalla completa. Tras los tres arreglos, con ~135 muestras por sitio en movimiento: sin explicar 0,000-0,008 % de
  media, peor muestra 0,12 % (más un frame de carga al 1 % en Kakariko).
- **CPU:** 0,4-0,6 ms por frame en el hilo de traducción (mediana; Kakariko y Vah Ruta), 0,7 ms con los pases de sombra.
  No se ha movido a un hilo propio.
- **TAA activo:** funciona sin cambios (la variante de captura y la de vectores comparten push constants).
- **Pases de sombra:** se capturan (~0,95 M de triángulos más en Kakariko, los tres niveles de cascada) y quedan bien
  colocados: la comprobación sigue casando con ellos dentro. Los mapas de sombra se dibujan después del G-buffer, así
  que la estructura se construye ahora antes del primer draw de pantalla completa que lee un mapa de sombra
  (`8c42c9bc921ed716`), que es donde la fase 2 la necesita.
- **fps con y sin:** no medible con este equipo: sin límite, Kakariko da 360 fps (tope) con RT apagado, con captura y
  estructuras, y con la comprobación incluida.

**Sigue pendiente de la fase 1:** presupuesto de VRAM (arena fija de 384 MB; hasta 209 MB usados), claves en
`precompile.c`, hilo propio, un santuario de verdad, vista en pantalla, deduplicar lo que traen los pases de sombra
(cada proyector entra hasta cuatro veces: escena y tres cascadas; trazar la comprobación sube de 5 a 10-14 ms) y los
~185 draws del pase de escena que llegan después de construir la estructura.

**Fase 2, primer corte** (`SAVAGEWIND_RT_SHADOWS`):

- **Dirección del sol:** de la cámara del mapa de sombras, ya capturada (su eje +z en ejes de mundo). No hizo falta
  descifrar constantes de luz.
- **Máscara:** un pase compute lanza un rayo por píxel hacia el sol desde el punto que da la profundidad, a la
  resolución del pase de sombra del juego (640x360 con 1280x720 interno). 0,1-0,8 ms de GPU.
- **Inyección** (`maxwell_rt_shadow`, `translate.c`): los tres shaders de sombra del sol del juego, reconocidos por
  hash, pasan sus lecturas comparadas de las cascadas por una función que devuelve la máscara. El resto de lo que
  calculan (desvanecidos, sombra de nubes) no se toca. Variante de traducción propia, como el arreglo de hierba mojada.
- **Modos:** 1, todo proyecta y sustituye a las cascadas; 2, solo proyecta lo que no lleva alfa-test y se queda lo más
  oscuro entre rayo y cascada; 3, como 2 sin cascadas (para mirar).

**Resultado visto** (Meseta, capturas en `player-tests\rt-phase0\sh\`): las sombras trazadas salen en la imagen del
juego, nítidas y bien orientadas; con el sol invertido todo queda en sombra, que confirma el sentido. Modo 1: la
sombra de un árbol es la silueta de sus tarjetas de follaje enteras (no hay alfa-test en el rayo). Modo 2: se parece al
juego con sombras de contacto más nítidas en roca y grietas. Modo 3: roca y terreno con sombra propia limpia, sin
sombra de follaje.

**Falta de la fase 2:** alfa-test en el rayo (leer la textura del draw en el impacto: es lo que decide si el modo 1
vale), penumbra (cono según el tamaño del sol y denoise temporal: ahora el borde es duro y a media resolución), los
lectores en forward (agua, terreno, hierba y la luz volumétrica siguen con las cascadas), qué pasa de noche y en
interiores (sin cámara de sombra no hay sol: se queda la sombra del juego), medir contra las cascadas apagadas, y
probar en más sitios que la Meseta y Kakariko.

## Fase 2, segundo corte: híbrido con el follaje y penumbra (2026-11-26)

- **Modo 4 (`SAVAGEWIND_RT_SHADOWS=4`), el que resuelve el follaje sin alfa-test en el rayo.** Lo opaco proyecta por
  rayos. Un rayo que encuentra una malla con alfa-test termina ahí y deja la decisión a las cascadas del juego, que
  tienen todos los proyectores y el recorte de la textura. Donde no hay follaje en el camino, las cascadas no se usan:
  ni su acné ni su corte entre niveles. La máscara lleva en `.b` qué parte de los rayos pasó por follaje.
- **Penumbra:** `SAVAGEWIND_RT_SHADOW_RAYS` rayos por píxel (4 por defecto) repartidos en un cono del tamaño del sol
  (`SAVAGEWIND_RT_SUN_SIZE`, 0,008 rad), girados por píxel, y el shader del juego lee la máscara con un filtro 3x3.
  Sin acumulación temporal todavía.

**Coste del pase de sombra** (640x360, 4 rayos, RTX 3080 Ti): Meseta 1,9 ms, Kakariko 2,2 ms, Hateno 1,8 ms, Bosque
Kolog 1,2 ms, Vah Ruta 0,7 ms. Antes de cortar el rayo en la primera tarjeta de follaje, la Meseta costaba 3,4 ms; con
un rayo, 0,8 ms.

**Visto** (capturas `*-m0.png` original y `*-m4.png` en `player-tests\rt-phase0\sh\`): en la Meseta la sombra del
árbol recupera la forma del follaje; Kakariko y Hateno salen coherentes con el original, sin artefactos a la vista.
Juzgado sobre capturas quietas a 1280x720: falta verlo en movimiento y jugando.

**Falta:** alfa-test real en el rayo (quitaría la dependencia de las cascadas para el follaje), acumulación temporal
(con 4 rayos la penumbra tiene 5 niveles), lectores en forward, noche e interiores, y el coste de fps real en una GPU
que no vaya sobrada.

## Fase 2, corrección: quién proyecta (2026-11-26)

El usuario vio la hierba alta peor que con RT apagado. Causa: proyectaban todas las mallas de la escena, también la
hierba, que el juego deja fuera de sus mapas de sombra; cada brizna sombreaba a la de al lado y el prado salía oscuro
y con ruido. Dos cambios:

- **Solo proyecta lo que el juego dibuja en sus mapas de sombra** (máscara de instancia 2; las mallas de la escena, 1,
  quedan para la comprobación). Es la elección de proyectores del propio juego. `SAVAGEWIND_RT_CASTERS=3` vuelve a
  incluir la escena. De paso el pase baja de 1,9 a 1,0-1,4 ms en la Meseta.
- **Distancia mínima del proyector** (`SAVAGEWIND_RT_TMIN=<cm>:<por mil de la distancia>`, por defecto 50:10). Los
  mapas de sombra usan a veces otra malla que la de pantalla (otro nivel de detalle), y una superficie que queda justo
  debajo de la versión de sombra se sombreaba a sí misma: las paredes de roca salían oscuras. Con 50 cm más el 1 % de
  la distancia la Meseta queda como el original. Coste: se pierden las sombras de contacto más cortas que eso, que
  eran parte de la gracia. Solo se probó ese valor.

Con las dos correcciones, la captura de la Meseta es casi indistinguible del original (`pl-m0b.png` contra
`pl-m4-t50.png`): la hierba limpia, la sombra de Link y la del árbol en su sitio. Lo que el trazado aporta ahí es
poco visible; habrá que buscar las escenas donde las cascadas fallan (acné, corte entre niveles, sombras lejanas)
para saber si compensa.

## Fase 2, sombras suaves y prueba a 1440p (2026-11-26)

Pedido del usuario: probar a 2560x1440 con el preset Savage al 100 % de resolución y la cámara en movimiento, y que
las sombras trazadas sean más suaves, que se fundan.

- **Penumbra de verdad:** el cono pasa a 0,02 rad (cuatro veces el sol real) con 6 rayos por píxel, así el borde se
  abre con la distancia al proyector. La máscara cruda guarda la profundidad y un segundo pase la filtra 5x5 solo
  entre texels de profundidad parecida (sin que la sombra de algo cercano manche lo de detrás).
- **Margen de distancia mínima, corregido otra vez:** el 50:10 de antes borraba la sombra de Link sobre la hierba a
  sus pies. Queda en 15 cm más el 0,4 % de la distancia.
- **Cómo comparar:** dos corridas distintas no sirven a 1440p (cargan a ritmos distintos y la luz del amanecer cae en
  frames distintos; las primeras comparaciones de esta sesión mezclaron estados y no valen). `SAVAGEWIND_RT_AB=<n>`
  alterna trazado y sombras del juego cada n frames dentro de una corrida y lo anota en el log.

**Visto** (`player-testst-phase0\cam\pl-ab-0_0\` y `pl-ab-15_4\`, frames 5010 sin trazado y 5070 con él, misma
escena): la sombra del árbol sobre la roca y la de Link sobre la hierba salen más suaves y fundidas con el trazado; la
roca y la hierba, iguales. Con la cámara girando hay capturas alternas (5370 a 5550) pero no se han comparado una a
una, y una foto no dice si la penumbra parpadea.

**Coste a 1440p Savage 100 %** (RTX 3080 Ti, máscara 1280x720, 6 rayos): sombras 3,2-3,6 ms de GPU; estructuras
1,3 + 0,27 ms; 1,5 ms de CPU en el hilo de traducción. Con límite de 60 fps se mantuvo en 59,9-60 al final de las
corridas, con tramos a 56-58 durante cargas. No es barato: a 120 fps serían unos 5 ms de 8.

## Fase 3, primer corte: reflejos trazados (2026-11-26)

Pedido del usuario: que el agua refleje el mundo, y también las superficies y armas metálicas.
`SAVAGEWIND_RT_REFLECT=1`.

**Cómo dibuja el juego el agua.** No es un pase forward transparente: la superficie va al G-buffer (por eso ya estaba
en la profundidad y en la estructura) y la ilumina uno de los nueve shaders de composición por clase de material.
Saltándose cada uno y midiendo qué zona cambia: el del agua es `a8fbe9863c41ff3b`. Tres de los nueve
(`f607b5ab4c2f0720`, `83398146712c7a25`, `a8fbe9863c41ff3b`) sacan el reflejo especular del cubemap de entorno, a un
nivel de detalle que sigue la rugosidad; los otros seis usan otra sonda (array 2D).

**Diseño:**

- Un pase compute lanza, a media resolución, un rayo por píxel en la dirección de la vista reflejada en la normal
  del G-buffer (target 3, RGBA8 `n * 2 - 1`, en ejes de mundo: comprobado con una vista de depuración).
- **Color del impacto: la escena iluminada del frame anterior** (opción A del problema 3). El punto de impacto se
  proyecta con la cámara del frame anterior y toma el color que tenía en pantalla. Lo que no estaba en pantalla, o
  un rayo que no toca nada, deja el cubemap del juego. Se desvanece hacia los bordes de la pantalla.
- Los tres shaders de composición pasan sus lecturas del cubemap por una función que mezcla el color trazado según
  el nivel de detalle que pedían: lecturas nítidas (agua, metal pulido) lo toman entero; las borrosas, el cubemap.
  Así es el propio juego quien decide cuánto refleja cada superficie.

**Visto** (`player-tests\rt-phase0\water\stable-off.png` contra `stable-refl.png`, 720p; y a 1440p Savage en
`cam\st-refl\`): el río del Establo de la Ribera pasa de un gris plano a reflejar la orilla, la arboleda y el puente.

**Coste a 1440p Savage 100 %** (RTX 3080 Ti): con el reflejo a resolución completa, sombras + reflejos eran 8,6 ms de
GPU y la corrida bajaba a 45-52 fps de media con límite de 60. A media resolución, 5,0 ms en total y 59,9 fps de media
(mínimo 40 en cargas).

**Límites y sin verificar:**

- **Metal y armas: no comprobado.** El mecanismo los cubre si su clase de material es una de las tres que leen el
  cubemap; no se ha buscado una escena con un arma metálica para verlo, ni se sabe qué clase usan.
- El color sale de la imagen final del frame anterior (10 bits, ya con tono aplicado y con el HUD dibujado): puede
  reflejar elementos del HUD y su brillo no está en la misma escala que el cubemap. A la vista en el río no se nota.
- No se comprueba que el punto de impacto fuese visible en el frame anterior: un reflejo puede coger el color de algo
  que lo tapaba.
- Un frame de retraso, sin reproyección de lo que se mueve; el follaje refleja como tarjetas enteras.
- Solo mirado en un sitio, en fotos quietas. La primera versión copiaba una textura equivocada (el agua salía morada).

## Fase 3, Dominio Zora y qué refleja y qué no (2026-11-26)

Pedido del usuario: probar en el Dominio Zora, cuyo suelo es piedra reluciente.

- **Primera prueba: ningún cambio.** Con lluvia el juego no dibuja los tres shaders de composición con cubemap sino
  sus tres versiones "mojado" (`970f06ddc0d6fa0a`, `b19a04739cefe2b2`, `dae9df4164496f40`, las del arreglo de hierba
  mojada), que no estaban en la lista. Añadidas: el suelo mojado refleja ahora las lámparas y las rocas
  (`player-tests/rt-phase0/water/zora-off.png` contra `zora-r1.png`).
- **Los otros seis shaders de composición no tienen reflejo de entorno.** Lo que parecía una sonda (un
  `sampler2DArray`) se lee con las coordenadas de pantalla: es un buffer de luz. Los materiales de esas clases no
  reflejan nada en el juego original, y por tanto tampoco con esto: el mecanismo actual solo mejora un reflejo que el
  juego ya hacía. Entre ellos está la clase de los personajes (`ec9c39a52f0aac1e`).
- **Consecuencia para metal y armas:** reflejarán solo si su material cae en una de las clases con cubemap. Para las
  demás habría que **añadir** el reflejo, y eso exige saber qué canal del G-buffer guarda el brillo o la rugosidad de
  cada píxel (targets 0, 2 y 4 sin descifrar). Sin hacer.
- `SAVAGEWIND_RT_REFLECT=2` (diagnóstico): el color trazado entero, sea cual sea el nivel de detalle que pedía la
  lectura del cubemap.

## Fase 3, brillo del material y reflejo añadido (2026-11-26)

**G-buffer descifrado en lo que hace falta.** Los shaders de composición leen del G-buffer solo las normales (target
3), el albedo (target 1) y la profundidad. El alfa del albedo, por 255, lleva en sus **3 bits altos el nivel de brillo
del material** (0 a 7): el shader lo convierte en su exponente especular, 2 elevado a (nivel - 2). Los 5 bits bajos,
sin descifrar. Vista de depuración: `SAVAGEWIND_RT_REFL_DEBUG=4`. En el Dominio Zora: suelo y roca nivel 0, Link
nivel medio, la forja ornamental zora nivel alto. El target 0 son bits de material (Link aparte); el 4, otro vector
tipo normal.

**Reflejo añadido** (`SAVAGEWIND_RT_REFL_ADD=<fuerza>`, 0,5 por defecto, 0 lo quita): cinco de las seis clases sin
reflejo de entorno (`ec9c39a52f0aac1e`, `885045460b17b7fb`, `966dae9aeabc743a`, `513a7bc28dd8faf8`,
`93be9efe796c375b`) suman al final el color trazado, pesado por el brillo del píxel (nada por debajo del nivel 3,5,
todo desde 6,5). La sexta (`e107910420e44c9f`) no lee el brillo y se deja.

**Visto y no visto:** en el Dominio Zora el efecto del reflejo añadido es mínimo: lo único con brillo alto en pantalla
son los filetes de la forja, de pocos píxeles. El suelo refleja por la vía del cubemap (clases mojadas), no por esta.
**No se ha comprobado un arma metálica**: la partida de prueba lleva una rama. Los umbrales y la fuerza están puestos
a ojo, y el color añadido viene de la imagen final (no está en la escala de la luz del juego ni teñido por el metal).

## Fase 3, personajes y equipo metálico (2026-11-26)

Prueba con la partida del 100 % (copia en `player-tests/rt-phase0/save100`; la original no se toca): Link con
equipo metálico, arco y espada a la espalda.

- **El "brillo" del alfa del albedo no sirve en personajes:** Link y todo lo que lleva tienen un único nivel (2). En
  la clase de los personajes esos bits no distinguen metal de tela.
- **El alfa de las normales (target 3) es la máscara especular en personajes:** arco, espada y herrajes salen
  blancos, tela y piel oscuros. También el pelo sale blanco (su brillo propio).
- **Reflejo en personajes:** la clase `ec9c39a52f0aac1e` suma el color trazado pesado por esa máscara (nada por
  debajo de 0,55, todo desde 0,9) en lugar del nivel de brillo.

**Visto** (`metal/m100-off-on.png`, recorte ampliado, de noche, fuerza 1,0): el arco y el arma cogen algo del color
del entorno, un matiz verdoso y azulado que sin reflejo no está. Es sutil: Link ocupa poco, es de noche, la espada
va envainada y el equipo se ve por detrás. **No hay todavía una foto donde se vea una hoja metálica reflejando algo
reconocible.** El pelo entra en la máscara y recibe reflejo sin deber; no se ha excluido.

## Fase 3, agua de cerca y reflejo de Link (2026-11-26)

Pedido del usuario: desenvainar la espada, y ponerse al borde de un lago a ver si se refleja Link. Partida del
100 %, Establo de la Ribera, mediodía (`SAVAGEWIND_TEST_TIME=12`), Link andando hasta meterse en el río. Comparación
con dos corridas completas (RT apagado contra reflejos), capturas en `player-tests/rt-phase0/lake/full-off.png` y
`full-on.png`, y `sword/water-off.png` y `water-on.png`.

- **De cerca el agua del juego ya refleja el entorno** (cielo, hierba, la tienda), en suave. La primera comparación
  en el río lejano, donde el agua original se veía gris plana, daba una idea exagerada de la mejora.
- **Con reflejos trazados sí aparece el reflejo de Link** bajo él, y los de los postes y las balas de heno, nítidos.
  Sin ellos, de Link solo se ve su sombra.
- **Pero el agua queda peor en conjunto en estas tomas:** pierde el azul del cielo y sale más oscura y a manchas.
  Causas probables, sin comprobar: el rayo sustituye la lectura del cubemap por el color de la orilla que tiene
  delante (correcto en geometría, pero sin la mezcla suave que hace el juego), el color viene de la imagen final ya
  con niebla y tono, y la normal del agua mete ruido de un píxel a otro sin filtrar.
- **La espada:** el botón de ataque la desenvaina, pero en las tomas queda oculta (hierba alta en una, bajo el agua
  en la otra). Sigue sin haber una foto de una hoja metálica reflejando.

**Conclusión provisional:** los reflejos trazados aportan lo que al juego le falta (objetos y personajes concretos en
el agua) pero, tal como están, estropean lo que el juego ya hacía bien. Antes de darlos por buenos hace falta filtrar
el resultado (espacial y temporal) y mezclarlo con el reflejo del juego en lugar de sustituirlo.

## Fase 3, arreglo del agua (2026-11-26)

Tres cambios tras ver que el reflejo trazado estropeaba el agua de cerca:

- **Mezcla, no sustitución:** el color trazado entra como mucho al 70 % sobre la lectura del cubemap; el reflejo suave
  del cielo y la tierra que ya hacía el juego queda debajo.
- **Solo lo cercano:** el impacto cuenta entero hasta 40 m y se desvanece hasta 160 m. Una ladera lejana ya no
  sustituye al cielo del cubemap; lo próximo (Link, postes, objetos, la orilla) sí se refleja.
- **Filtro al leer:** cuatro lecturas bilineales del color premultiplicado por su confianza.

**Visto** (`player-tests/rt-phase0/shore/`, partida del 100 %, mediodía): con Link metido en el agua (`front-on.png`)
el agua vuelve a verse limpia y azul y conserva su reflejo. Con Link en la orilla, casi fuera del agua, y la cámara
del lado del agua (`w383-on.png` contra `w383-off.png`, y el recorte `w383-off-on-crop.png`): con RT se ve el reflejo
invertido de Link y de la orilla de hierba; sin RT, solo su sombra.

**Pegas:** las dos corridas no dejan a Link en el mismo punto exacto (en una queda más metido), así que la comparación
vale para "hay reflejo o no", no píxel a píxel. En el recorte ampliado se nota el patrón de la media resolución en el
reflejo de la hierba. Sin acumulación temporal: no se sabe si tiembla con el oleaje.

## Fase 3, acumulación temporal del reflejo (2026-11-26)

- Cada frame se traza un píxel distinto de cada bloque 2x2 (en cuatro frames, todos), y el resultado se mezcla al
  20 % con el del frame anterior, leído donde ese punto de la superficie estaba en pantalla entonces (misma matriz
  que el color del impacto). Fuera de pantalla no hay historia y se usa el frame tal cual.
  `SAVAGEWIND_RT_REFL_ACCUM=0` lo quita.
- **Visto** (`player-tests/rt-phase0/shore/acc-before-after-crop.png`): el patrón de la media resolución desaparece y
  el reflejo queda continuo. `acc-move.png`: Link fuera del agua en la orilla tras girar la cámara 200 frames, sin
  artefactos a la vista en la foto.
- **Coste a 1440p Savage 100 %** con sombras y reflejos: 5,0 ms de GPU (igual que antes de acumular), 0,9 ms de CPU,
  59,9 fps de media con límite de 60 (mínimo 44 en cargas).
- **Sin comprobar:** estelas. Con un 20 % por frame, algo que se mueve deja su reflejo anterior unos cinco frames, y
  no hay rechazo de historia. Solo se ve jugando.

## De dónde partimos

Lo que el renderer ya sabe de BOTW, y que hace esto abordable:

- **El juego es diferido.** El pase de geometría escribe un G-buffer de 6 targets (albedo en el 1; sus shaders escriben
  0, 1 y 3, algunos el 5), una D16 con el ID de material y la profundidad; la composición de luz lee normales, albedo,
  profundidad lineal, la probe (cubemaps 64x64 R11G11B10) y los buffers de luz. Después va un pase forward de efectos y
  transparentes. Para RT híbrido esto es lo mejor que podía pasar: los rayos primarios no hacen falta, salen del
  G-buffer.
- **Identidad de cada draw e historia de sus constantes** (`vulkan/mvec.c`): cada draw de escena tiene clave (shaders,
  streams de vértices, rango de índices) y huella de sus constantes; casa con el frame anterior en el 93-99 %.
- **Variantes de shader desde el traductor** (`maxwell/translate.c`, `ShaderKey.mvec`): ya se emite una segunda copia
  del vertex shader de cada draw de escena con otras entradas. Una variante más, "captura", es el mismo mecanismo.
- **Hechos medidos en el spike de vectores:** ningún draw de escena tiene vértices reescritos por la CPU; todo lo que
  mueve un vértice está en constant buffers (también las paletas de huesos) o en texturas que lee el vertex shader
  (alturas de terreno, viento; ~la mitad de los draws); 2.300-3.900 draws de escena por frame.
- **Cámara** (`mvec_camera`, `mvec_camera_now`): matrices actual y anterior, near/far, FOV, jitter.
- **Cascadas del sol identificadas** (`RS_SHADOW`, `vk_is_shadow_copy`, `vk_shadow_direct`): el runtime ya sabe qué
  pases son sombras y quién las lee.
- **Denoisers y reconstrucción a mano:** TAA propio con vectores por objeto, DLSS (y su descarga en el PC del jugador),
  FSR 3, XeSS.

Lo que falta por completo: ninguna extensión de RT en `device.c` (`VK_KHR_acceleration_structure`,
`VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, buffer device address), transform feedback sin implementar, y
ningún conocimiento de la escena en espacio de mundo: el runtime solo ve draws cuyo vertex shader (código del juego)
saca posiciones en clip space.

## Los cuatro problemas de verdad

### 1. Geometría en espacio de mundo

Un BLAS necesita triángulos en un espacio fijo; el juego entrega vértices crudos más un programa que los transforma.

**Propuesta: captura por compute.** Para cada vertex shader de escena, el traductor emite una variante compute: el
mismo cuerpo, ejecutado por vértice (no por triángulo rasterizado), que escribe la posición a un buffer. La posición
de clip antes de dividir por w es función lineal de la de mundo, así que `inv(viewProj) * clip` la recupera sin la
pérdida de precisión de la profundidad ya dividida (la matriz es la de `mvec_camera`; se trabaja relativo a la cámara
para no perder bits lejos del origen). Junto a la posición se capturan los varyings que haga falta para el material
(UV; ver problema 3).

La escena medida es pequeña (0,5-0,8 M de triángulos por frame, 0,25 M en mallas distintas), pero reconstruirlo todo
cada frame sigue siendo trabajo tirado. Reparto:

- **Estático** (terreno, edificios, rocas, árboles sin viento): se captura una vez por identidad de draw, BLAS
  construido y compactado, se reutiliza. Como las constantes llevan matrices dependientes de la vista (el 99 % cambian
  al mover la cámara), "no ha cambiado" no se decide por la huella de constantes sino por el resultado: unas pocas
  posiciones de mundo de muestra por draw, comparadas con las guardadas.
- **Rígido en movimiento** (objetos empujados, armas, plataformas): mismo BLAS, cambia la transformación de su
  instancia en el TLAS (transformación ajustada a las muestras; si no es rígida, pasa a deformable).
- **Deformable** (personajes con huesos, tela, hierba al viento): recaptura y *refit* por frame, con presupuesto
  (los más cercanos primero; el resto cada N frames).
- **Hierba e instanciados masivos:** fuera del BLAS al principio (no proyectan sombra trazada, siguen recibiéndola);
  alfa-test en any-hit es lo más caro de RT y la hierba es el peor caso.
- TLAS reconstruido cada frame (barato: miles de instancias).

Todo el trabajo de CPU (decidir, casar, programar builds) va en un hilo propio, como los vectores desde el
2026-11-13: en el hilo de render no cabe (ya es el cuello de botella).

### 2. Lo que la cámara no ve

El juego descarta por frustum y por distancia en CPU: lo que queda detrás de Link o fuera de plano no se dibuja, así
que no existe para un rayo. Es el límite principal de calidad, sobre todo para reflejos y GI. Mitigaciones, por orden:

1. **Los pases de sombra también se capturan.** Las cascadas dibujan desde el sol, con su propio frustum: traen los
   proyectores de sombra fuera de cámara. Para sombras del sol trazadas esto cubre casi todo.
2. **Caché persistente:** un BLAS estático no se tira cuando su draw deja de llegar; vive mientras esté dentro de un
   radio de la cámara y quepa en el presupuesto de VRAM. Al girar la cámara, lo ya visto sigue ahí.
3. **Recurso de pantalla:** un rayo que no toca nada usa la probe del juego (cielo/ambiente) como hasta ahora.
4. **A investigar, no comprometido:** ensanchar el culling del juego con un parche (como `draw_distance.c` hace con el
   presupuesto de actores; cuesta CPU del juego), o usar el mundo de colisión que el juego ya tiene en memoria como
   geometría gruesa fuera de cámara.

### 3. Material en el punto de impacto

En el primer impacto (el píxel) el G-buffer lo da todo. En un impacto secundario (lo que se ve en un reflejo, lo que
rebota la luz) solo hay un triángulo. Opciones:

| Opción | Calidad | Coste |
|---|---|---|
| A. Si el impacto cae en pantalla, usar el color ya iluminado del frame anterior (reproyectado) | Buena donde aplica | Bajo |
| B. Proxy por draw: UV capturados + la textura de albedo del draw (la que su fragment shader escribe al target 1; se identifica por shader) | Media: sin capas, sin mezcla de terreno | Medio; un array de texturas bindless |
| C. Ejecutar el fragment shader del juego como hit shader | Exacta | Muy alto: ~1.850 shaders de G-buffer más como variantes, todos los varyings capturados, sin derivadas para mips. Descartado salvo para un conjunto pequeño (terreno, agua) |

Propuesta: A + B, con el terreno como caso especial si B no basta. Para sombras y oclusión ambiental este problema no
existe (solo importa si el rayo toca algo); por eso van primero.

### 4. Meter el resultado en la imagen del juego

La luz la compone el juego con sus shaders. Hay que sustituir términos concretos:

- **Sombra del sol:** el shader que lee las cascadas recibe en su lugar una máscara de sombra trazada (variante del
  traductor, igual que `vk_shadow_direct` ya le cambia la fuente). Las cascadas dejan de hacer falta para la vista
  (ahorro), pero su pase sigue sirviendo de fuente de geometría, a menor resolución.
- **Ambiente / probe:** la lectura de la probe en la composición pasa a un buffer de irradiancia trazada.
- **Reflejos:** se suman en el pase forward o tras la composición, pesados por la rugosidad del G-buffer.
- **Estilo:** BOTW sombrea personajes con rampa cel. El ID de material de la D16 permite dejar la rampa intacta y
  cambiar solo su entrada (sombra, ambiente). Un path tracer que ignore esto cambia el arte del juego: no se hace.

Falta por descifrar (fase 0): codificación exacta de normales, dónde está la rugosidad/especular, qué shader lee las
cascadas, dirección y color del sol y lista de luces locales en los buffers de luz.

## Fases

Cada fase termina con números (fps a 60/120/sin límite, ms de GPU, VRAM, antes/después intercalado) y una vista de
depuración. Todo detrás de `SAVAGEWIND_RT=1` hasta que una fase esté para jugadores.

**Fase 0 — Medir (spike, sin RT). Hecha el 2026-11-26: ver "Fase 0: medidas".** Instrumentación desechable como la del spike de vectores:
vértices y triángulos por frame y por clase (estático / rígido / deformable / instanciado), cuántos BLAS únicos, cuánta
geometría añaden los pases de sombra, VRAM estimada; descifrar el G-buffer y los buffers de luz (lista de arriba).
Salida: este documento con las estimaciones sustituidas por medidas, y decisión de seguir o no.

**Fase 0.5 — Espacio de pantalla (opcional, independiente).** GI y reflejos por ray marching sobre profundidad y
color, sombras de contacto. Sin extensiones, sirve en todas las GPU (también Deck). Obliga a resolver el problema 4
(dónde inyectar) con poco riesgo, y queda como nivel "bajo" de la opción final.

**Fase 1 — Infraestructura RT.** Extensiones y detección (`gpu_select.h`, `device.c`; sin RT la opción no aparece),
variante de captura en el traductor, caché de BLAS, TLAS por frame, hilo propio, liberación diferida
(`vk_defer_*`), presupuesto de VRAM (`vram.c`). **Criterio de paso:** vista de depuración que traza rayos primarios y
compara su profundidad con la del G-buffer; error bajo en Meseta, Kakariko, Bosque Kolog y un santuario, en parado y
en movimiento. Entran las claves nuevas en `precompile.c` para no compilar en juego.

**Fase 2 — Sombras del sol trazadas.** Un rayo por píxel hacia el sol (cono según su tamaño angular: penumbra real),
`ray query` en compute, denoise temporal con los vectores que ya hay. Primera fase visible para jugadores. Sustituye
el acné, el peter-panning y el corte entre cascadas.

**Fase 3 — Oclusión ambiental y reflejos.** RTAO a media resolución. Reflejos en agua y superficies lisas con
material A + B.

**Fase 4 — GI difusa de un rebote.** Sondas de irradiancia en rejilla alrededor de la cámara (tipo DDGI: estable,
barato, tolera bien geometría incompleta) antes que GI por píxel. Sustituye la probe ambiental del juego.

**Fase 5 — Path tracing.**
- *Modo foto:* cámara quieta, acumulación de N rebotes con sol, cielo, luces locales y emisivos; sin denoiser.
- *Tiempo real experimental:* visibilidad primaria del G-buffer, 1 muestra por píxel, ReSTIR para luz directa e
  indirecta, y un denoiser: DLSS Ray Reconstruction (NVIDIA; su DLL se descargaría en el PC del jugador como la de
  DLSS, nunca incluida) o NRD (todas las marcas; revisar su licencia contra la MIT del proyecto antes de vendorizar).
  El pase forward de efectos y transparentes sigue rasterizado encima.

## Hardware y coste (estimado, a confirmar en fase 0)

- **Requisito:** Vulkan con `ray_query`: RTX 20 o superior, RX 6000 o superior, Intel Arc. La Steam Deck lo expone
  por RADV pero no tiene potencia: solo fase 0.5. macOS/Metal (sin mantener): no. La ruta Vulkan 1.2 de hardware
  antiguo no se toca.
- **Tiempo de GPU** a 1440p en una gama RTX 3080 Ti: sombras ~1-2 ms; AO + reflejos ~2-4 ms; GI por sondas ~1-2 ms;
  path tracing en tiempo real 12-25 ms más el denoiser. Con el objetivo de 8 ms / 120 fps, las fases 2-4 caben
  combinadas con un upscaler; el path tracing en tiempo real es un modo de 30-60 fps.
- **VRAM:** con la escena medida, 20-30 MB para lo visible y 100-200 MB para una caché de todo lo visto en un minuto
  (a 60-100 bytes por triángulo compactado, sin medir). Cabe en tarjetas de 8 GB; en 4-6 GB, RT apagado.
- **CPU:** el objetivo es <0,3 ms añadidos al hilo de render; el resto en su hilo.
- **Shaders:** una variante de captura por vertex shader de escena (~1.100) más las de inyección; alarga la
  precompilación solo con RT activado.

## Riesgos

1. La captura no reproduce la escena (vertex shaders con trucos de clip space, depth bias, billboards): se ve en la
   fase 1 y se excluyen por shader.
2. Geometría fuera de cámara insuficiente: reflejos y GI con huecos al girar. Acotado por la caché y el recurso de
   pantalla; no resuelto del todo sin tocar el culling del juego.
3. Coste de refit de personajes en escenas llenas (Kakariko, establos).
4. Vegetación con alfa: o no proyecta sombra trazada, o cuesta mucho. Puede obligar a mezclar cascadas (vegetación)
   con rayos (lo demás).
5. Drivers: RT añade otra superficie de fallos por marca; los builds asíncronos y las estructuras en vuelo tienen que
   respetar el modelo de sincronización de `vk.h`.
6. Mantenimiento: cada shader parcheado por hash es una dependencia de la versión del juego (objetivo 1.9.0).

## Qué no se hará

- Incluir, alojar o enlazar geometría, texturas o datos del juego: todo se captura en ejecución del volcado del
  jugador.
- Copiar de proyectos de decompilación para entender formatos de escena.
- Activarlo por defecto o cambiar la imagen de quien no lo encienda.
- Poner las opciones en los menús del juego: van en Video del menú de Savage Wind.

## Fase 2/3, prueba del usuario jugando (2026-11-26)

Prueba visible a 1440p, preset Savage, RTX 3060. Tres observaciones y lo que resultó de cada una.

**Dos sombras por personaje (una suave, una nítida).** Reproducido en el Rancho del Río a las 17 h con la cámara
desde arriba. Causa: en el modo híbrido los rayos que cruzan una malla con textura recortada dejan la decisión a las
cascadas del juego. Link tiene piezas sólidas (sombra de los rayos, suave) y piezas recortadas (pelo, bordes de tela:
sombra de las cascadas, nítida), así que proyectaba las dos. Arreglo: una malla recortada de las pasadas de sombra
cuyo vertex shader lee una paleta de huesos (constant buffer 5 de un kilobyte o más) proyecta entera en los rayos. Las
hojas de los árboles, que no tienen huesos, siguen en las cascadas. `SAVAGEWIND_RT_NO_SKIN_CUT=1` vuelve a lo anterior.
Los shaders de fragmento de las pasadas de sombra se listan con `SAVAGEWIND_RT_SHADOW_FS=1`.

**Rayas / puntos en las sombras.** Las bandas de puntos de la captura del usuario (sombra del establo sobre el suelo
y el carro) no se han reproducido. Sí se vio un patrón regular en la penumbra, y se añadió acumulación temporal al
filtro de la máscara: el patrón de los rayos gira cada frame y el filtro mezcla un quinto del frame actual con lo que
ese punto de la superficie tenía en el anterior (reproyectado con la cámara anterior, rechazado si la profundidad no
coincide). La máscara filtrada pasa a RGBA16F y guarda la profundidad en `.a`. `SAVAGEWIND_RT_SHADOW_ACCUM=0` lo
quita. Queda un serrado de 2 píxeles en los bordes que no es ruido: el juego calcula su sombra a 1280x720 y la amplía
sin filtrar; con su sombra dura no se nota y con una penumbra suave sí. Arreglarlo pide encontrar la pasada que lee
ese buffer y darle la máscara filtrada a resolución completa.

**"No hay reflejos en el agua".** La captura es cenital: el rayo reflejado sube al cielo y no toca nada, así que ahí
se ve el cubo del juego, como sin ray tracing. En ángulo rasante sí hay reflejo (las rocas de la orilla y el propio
Link en la charca del rancho, comparado en una misma corrida con `SAVAGEWIND_RT_AB`). El peso del reflejo sigue la
reflectancia que el juego da al agua, baja al mirar hacia abajo.

Con el sol bajo los rayos sombrean más terreno que las cascadas (laderas enteras a lo lejos), porque alcanzan 4 km
y las cascadas no. No se ha comprobado contra una referencia cuál de las dos acierta.

## Serrado de la sombra y reflejos de espejo (2026-11-26, segunda ronda)

**Serrado de 2 píxeles.** El juego ilumina la escena a mitad de tamaño (buffer de luz de 1280x720 a 1440p) y sus
pasadas de composición toman, para cada píxel, la luz de uno de los cuatro texels de alrededor, el más cercano en
profundidad. Con su sombra dura eso no se ve; con la penumbra trazada salían dientes de dos píxeles. Las doce pasadas
de composición (`maxwell_rt_comp_pass`) pasan ahora sus lecturas de texturas del tamaño de la máscara por `rt_lb`:
donde los cuatro texels están a la misma profundidad (la `.a` de la máscara) se interpolan, y donde no, vale la
elección del juego. Solo con sombras trazadas; `SAVAGEWIND_RT_LIGHT_SMOOTH=0` lo quita. Las pasadas que leen el
objetivo de la sombra diferida se listan con `SAVAGEWIND_RT_MASK_READERS=1`. Visto en el Rancho del Río: la sombra de
Link y la de la valla salen sin dientes. Coste sin medir por separado.

**Reflejos.** Tres cambios:

- El color de lo que toca el rayo se toma de la pantalla; si ese punto está tapado por algo más cercano, el color era
  el de lo que tapa. Ahora se compara la profundidad del punto con la del buffer en ese sitio y, si queda detrás, el
  rayo no cuenta y vale el cubo del juego.
- El alcance pasa de apagarse entre 40 y 160 m a hacerlo entre 1500 y 3000 m. El límite corto se puso por las manchas
  oscuras de laderas lejanas, que venían del punto anterior.
- La parte del reflejo nítido que toma el color trazado sube de 0,70 a 0,95 (`SAVAGEWIND_RT_REFL_MAX`).

El agua refleja como espejo donde está quieta (la charca del rancho: banderas, valla y poste invertidos). En el lago
el agua tiene oleaje en sus normales y el reflejo sale roto y estirado, como corresponde. Lo que no se ve en pantalla
(detrás de la cámara, fuera del encuadre, la cara oculta de las cosas) no tiene color que dar y ahí sigue el cubo del
juego: es el límite de tomar el color de la imagen. Quitarlo pide sombrear el punto tocado con su material.

El juego ya da por sí mismo un reflejo ondulado de lo cercano en esa charca; el trazado lo sustituye por uno con la
geometría real, y sale más oscuro cuando lo reflejado está en sombra trazada.

Nota de pruebas: al cambiar una opción que retraduce shaders la carga tarda más y la entrada con guion (por número de
frame) cae antes de tiempo; la segunda corrida con la caché caliente es la que vale.

## Agua sin reflejo y estela de la sombra (2026-11-26, tercera ronda)

Posición del usuario: `swr:1 x=-854.37 y=205.85 z=1901.39 yaw=-75.1 pitch=-44.3 hour=15.20` (estanque junto al Templo
del Tiempo). Las pruebas pasan a hacerse con el punto de control (`SAVAGEWIND_CONTROL`, `tools/swctl.py`: `goto`,
`look`, `shot`), que coloca a Link y la cámara exactamente; el guion por número de frame no era repetible.

**El agua no reflejaba el mundo.** Dos causas, vistas con vistas de depuración nuevas (`SAVAGEWIND_RT_REFLECT=3`: solo
el color trazado, magenta donde no hay; `SAVAGEWIND_RT_REFL_DEBUG=6`: hasta dónde llega cada rayo; 7 y 8: la normal y
la dirección de vista usadas):

- El juego dibuja el agua en el G-buffer después de las primeras pasadas diferidas, que es donde se construye la
  estructura y se trazaba. En ese momento el píxel del lago es el fondo, no la superficie, y el rayo salía del fondo.
  Ahora los reflejos se trazan en el volcado y otra vez justo después de esos dibujos tardíos (`rtr_reflect_late`,
  antes del primer dibujo a pantalla completa que sigue); la copia a la historia se hace una vez, antes de la primera
  pasada de composición (`rtr_reflect_done`). No sirve trazar en la composición: para entonces el juego ha reutilizado
  la textura de normales (media pantalla con otra cosa). `SAVAGEWIND_RT_REFL_LATE=0` vuelve a trazar solo en el volcado.
- Las normales del agua son un mapa de ondas de pocos grados; un rayo por cada una dispersaba el reflejo en vetas.
  Las superficies casi horizontales (normal.y > 0,94) llevan su normal 56/64 hacia la vertical antes de reflejar
  (`SAVAGEWIND_RT_REFL_FLAT=<0..63>`).

Resultado en esa posición: el acantilado, el islote y las rocas se ven invertidos en el agua. Límites: lo que no está
en pantalla no se refleja (mirando hacia abajo desde la orilla, el mundo reflejado queda fuera del encuadre, y la cara
de Link, que mira a la cámara, tampoco se ve desde el agua); el cielo sigue viniendo del cubo del juego.

Se probó leer las normales como espacio de vista (`SAVAGEWIND_RT_NORMAL_VIEW=1`, ahora por defecto) con las dos
orientaciones del eje vertical; la buena es derecha x adelante. `=2` fuerza la normal vertical, `=0` las lee tal cual.

**Estela de la sombra al moverse.** La acumulación de la máscara mezclaba un quinto por frame sin más, y al andar
quedaba la sombra anterior unos frames, en bloques del tamaño de la máscara. La historia se recorta ahora al mínimo
y máximo de los rayos de este frame en el vecindario 5x5: donde la sombra se ha ido no queda nada de ella, y en la
penumbra sigue promediando. Sin comprobar en movimiento: la prueba con guion hizo trepar a Link por la roca.

## Reflejo de lo que no está en pantalla: caché de color por triángulo (2026-11-27)

Hasta aquí el color de un reflejo salía solo de la imagen del frame anterior: un rayo que tocaba algo fuera del
encuadre, tapado por otra cosa o de espaldas a la cámara no devolvía nada y quedaba el cubo del juego.

Se descartó sombrear el punto tocado con su material (UV y textura de cada malla): el terreno, que es casi todo lo
que refleja un lago, mezcla capas de un array de texturas con UV derivadas de la posición, y habría que rehacer su
shader. En su lugar hay una **caché de color**, independiente del material:

- Cada malla guardada tiene 4 celdas por triángulo (centro y tres esquinas, por baricéntricas) en un buffer de
  170 MB, en el sitio que ya ocupa en los índices guardados (`instanceCustomIndex` = primer índice / 3).
- Un pase de cómputo (`k_fill_src`) lanza un rayo primario por cada píxel de media resolución (el mismo píxel del
  2x2 que el reflejo de ese frame). Si toca lo que dice el buffer de profundidad, la celda de ese triángulo toma el
  color iluminado que el punto tenía en el frame anterior, mezclado al 30 % con lo que tenía.
- El reflejo, cuando lo tocado no está en pantalla o está tapado, usa la celda (o la media de las otras celdas del
  triángulo). Lo que sí está en pantalla sigue saliendo de la imagen, que es más nítida.
- Las celdas de una malla nueva se ponen a cero al crearla.
- Solo en superficies casi horizontales (el agua). En roca y hierba el reflejo propio es un desenfoque del cubo, y
  lo que añadiera la caché volvería a entrar en ella frame tras frame.

`SAVAGEWIND_RT_REFL_CACHE=0` la quita; `SAVAGEWIND_RT_REFL_DEBUG=9` muestra solo la caché.

Comprobado en `swr:1 x=-854.37 y=205.85 z=1901.39 yaw=-75.1 pitch=-44.3 hour=15.20` (1440p, RTX 3060, ejecución
oculta): tras mirar una vez hacia arriba, con pitch -44 el agua refleja el acantilado que queda fuera del encuadre,
que antes no salía (`player-tests/rt-phase0/lago/cc-on-44b.png`).

Límites:

- **Solo lo que se ha visto alguna vez.** Una cara nunca mostrada a la cámara no tiene color. La malla se suelta a
  los 240 frames sin dibujarse y su caché con ella.
- **Solo lo que el juego dibuja.** Lo que el juego descarta por estar fuera del encuadre no entra en la estructura
  y ningún rayo lo toca. Arreglarlo pide tocar el descarte del juego.
- El color guardado es el de cuando se vio: luz, sombra y niebla de ese momento.
- 4 celdas por triángulo: en triángulos grandes de terreno el reflejo de lo no visible sale borroso.
- El lecho del lago guarda el color del agua (el pase lee la profundidad antes de dibujarse el agua). No se ha
  visto que estorbe.
- Coste sin medir bien: en dos ejecuciones ocultas no intercaladas, sombras + reflejos pasaron de 12.1 a 15.5 ms
  de GPU con la caché. Falta medirlo intercalado.

### Link no se reflejaba: lo dibujado después de construir la estructura (2026-11-27)

El juego dibuja unos 210 draws del G-buffer (32 000 triángulos: los personajes, Link entre ellos, y el agua) después
de los primeros pases diferidos, que es donde se construye la estructura del frame. Esos draws se contaban como
"left out, drawn after the structure was built" y ningún rayo los tocaba: el agua reflejaba el paisaje pero no a Link.

Ahora, mientras el reflejo espera su segundo trazado, esos draws también se capturan. `rtr_reflect_late` construye o
reajusta sus mallas, las añade a las instancias del frame, rehace el nivel superior y solo entonces traza. La caché
de color se rellena en ese segundo pase cuando lo hubo en el frame anterior (así ve también a Link y al agua), y en
el primero si no. `rtr_pending()` ya no cuenta esas capturas tardías, para que no disparen otro flush.

En superficies horizontales el rayo sale 12 cm por encima (`SAVAGEWIND_RT_REFL_LIFT`: 1 nada, 2 30 cm): el agua está
ahora en la estructura. En la prueba no cambió nada entre 0, 12 y 30 cm.

Comprobado con una cámara fija baja sobre el lago mirando a Link metido hasta la cintura
(`SAVAGEWIND_TEST_CAM="ex,ey,ez,ax,ay,az"`, gancho de prueba en `hle/freecam.c`): cara, pelo y túnica salen
invertidos en el agua junto a las ruinas (`player-tests/rt-phase0/lago/l8_crop.png`). Con la cámara normal del juego,
mirando hacia abajo, el reflejo casi no se ve: a ese ángulo el agua del juego apenas refleja (Fresnel), y es correcto.

Sin medir: el coste del segundo nivel superior y de los reajustes tardíos. No hay comparación antes/después en el
mismo encuadre.

## Opciones de Vídeo decididas (2026-11-27)

Decisión del usuario, tomando como referencia el menú de Control:

- **Organización:** un interruptor general de Ray Tracing y, debajo, cada efecto con su propio Off/On.
- **Efectos:** sombras RT (hecho), reflejos RT en agua (hecho), reflejos RT en superficies opacas (por hacer), luz indirecta RT (por hacer).
- **Calidad:** resolución del trazado (mitad o completa), muestras por píxel (1, 2, 4, 8), distancia de trazado, y reflejo de lo que está fuera de pantalla (Off/On, la caché de color de 170 MB).
- **Sin soporte:** las filas se muestran en gris con el texto "Tu gráfica no soporta ray tracing".

Todo va en el menú de Savage Wind, apagado por defecto.

## Menú de ray tracing: primera versión (2026-11-27)

Las filas están en Opciones avanzadas de Vídeo (pantalla de opciones del juego y menú F1): Ray tracing, Sombras, Reflejos, Reflejos de fuera de pantalla, Muestras (1/2/4/8) y Distancia (Cerca/Media/Lejos). Ajustes nuevos en `settings.h`: `rt`, `rt_shadows`, `rt_reflections`, `rt_offscreen`, `rt_samples`, `rt_distance`. Ray tracing viene apagado; con él encendido, sombras, reflejos y fuera de pantalla vienen encendidos, 4 muestras y distancia Lejos.

**Qué pide reinicio.** El interruptor general y los dos efectos se aplican desde el próximo inicio. Los efectos van dentro de los shaders del juego (variantes de las pasadas de sombra y de composición), que se traducen al arrancar, igual que "Corrección de hierba mojada" y "Estrellas"; no hay mecanismo para retraducirlos en marcha. Muestras y distancia se aplican al momento. "Fuera de pantalla" se apaga al momento, pero encenderlo pide reinicio porque su memoria (170 MB) se reserva al arrancar.

**Sin soporte.** `device.c` mira siempre si la gráfica puede (antes solo con `SAVAGEWIND_RT`), y las filas salen en gris con "Tu gráfica no soporta ray tracing". Las extensiones solo se piden si el ajuste está encendido.

**Variables de entorno.** Con `SAVAGEWIND_RT` definida mandan las variables, como antes, y los ajustes no cuentan: las pruebas automáticas no cambian. La pasada de diagnóstico (`SAVAGEWIND_RT_CHECK`) queda apagada cuando se enciende desde el menú.

**Medido** (corrida oculta, 1440p, Meseta, hora fija y cielo despejado, `gpuprof`, alternando valores en la misma corrida):

| Ajuste | GPU del frame |
|---|---|
| 1 muestra | 22,1 / 22,3 ms |
| 4 muestras | 27,0 ms |
| 8 muestras | 33,0 / 33,1 ms |
| Distancia Cerca frente a Lejos | 27,0 frente a 27,0 ms |
| Fuera de pantalla apagado | 24,7 ms (2,4 ms menos) |

Las cifras absolutas son de una corrida oculta con el escritorio en uso; valen las diferencias.

**Pendiente.**
- La distancia, tal como está (longitud máxima del rayo: 1000, 2000 o 4000 m), no ahorra nada. Para que sirva tiene que ser distancia desde la cámara: más allá, la sombra y el reflejo del juego.
- Las muestras solo afectan a las sombras; los reflejos trazan un rayo cada 2x2 píxeles.
- Resolución del trazado (mitad o completa): sin hacer.
- Las filas no se han visto en pantalla y el efecto visual de muestras y distancia no se ha juzgado a ojo.
- Solo hay traducción al español; el resto de idiomas muestra el inglés.

## Estado al publicar como trabajo en curso (2026-11-27)

Todo lo anterior está en `main` local y en `test` como un solo commit, "Ray tracing (work in progress)". Viene apagado: quien no toque Vídeo > Opciones avanzadas > Ray tracing no nota nada.

### Comprobado: con ray tracing apagado no cambia nada

**Por código.** Con el ajuste apagado (y sin `SAVAGEWIND_RT`) no se piden las extensiones, `rtr_device_ready` sale sin cargar nada y `rtr_on()` es falso, así que:

- Las variantes de shader (`maxwell_rt_shadow`, `maxwell_rt_lb`, `maxwell_rt_refl`, `maxwell_rt_radd`) quedan a 0: las pasadas de sombra y de composición del juego se traducen igual que antes y con la misma identidad en la caché. `ShaderKey` (56 bytes) y `ShaderInfo` (2248 bytes) miden lo mismo que en `main`; el campo nuevo `rt_cap` va en el relleno y vale 0.
- En `exec.c` cada enganche está tras `rtr_on()`, `rtr_reflections()`, `rtr_pending()`, `rtr_reflect_waiting()` o `rtr_reflect_open()`, todos falsos. No se captura geometría, no se crea ninguna imagen ni buffer del trazado y no se añade ninguna pasada.
- Los enlaces nuevos de descriptores (62, 95, 96, 97) solo se miran después de las texturas propias del shader, y ningún shader sin variante los declara.
- Lo único que corre siempre es la consulta de si la gráfica soporta ray tracing (para poner las filas en gris), que no cambia el dispositivo.

**Por prueba.** Cuatro corridas ocultas alternadas, dos con un exe de `main` puro y dos con el exe con ray tracing y el ajuste apagado, misma partida, 1440p, hora fija y cielo despejado:

| | main | con el código, apagado |
|---|---|---|
| Líneas `[rt]` en el log | 0 | 0 |
| Pipelines creados al cargar | 549 / 538 | 538 / 536 |
| Frame medio | 18,23 / 18,21 ms | 18,17 / 18,16 ms |
| Pasadas del frame capturado | 165 / 163 | 165 / 167 |

Las pasadas de la escena (mapas de sombra, G-buffer, sombra diferida, composición) son las mismas, con los mismos shaders. Las que cambian entre corridas son las del mapa de entorno y las lecturas de 1x1, que también cambian entre dos corridas del mismo exe. La diferencia media entre capturas es de 8,6 a 10,9 niveles entre los dos exes y de 7,9 a 8,0 entre dos corridas del mismo: las nubes, el destello del sol y la hierba al viento no se repiten, así que la comparación píxel a píxel no distingue más que eso.

### Para seguir

Por orden:

1. **Distancia**: rehacerla como distancia desde la cámara (más allá, sombra y reflejo del juego). Hoy limita la longitud del rayo y no ahorra nada.
2. **Muestras en los reflejos**: hoy solo cuentan para las sombras.
3. **Resolución del trazado**: mitad (lo de hoy) o completa.
4. **Reflejos en superficies opacas**: metal, hielo, suelo mojado; hay que leer la rugosidad del material.
5. **Luz indirecta**.
6. Ver el menú en pantalla y juzgar a ojo muestras y distancia; medir el coste de la segunda construcción del nivel superior por frame; mirar la mancha pálida sobre la roca lejana en el lago; comprobar la sombra en movimiento.

Pruebas: `SAVAGEWIND_RT=1` con `SAVAGEWIND_RT_SHADOWS=4` y `SAVAGEWIND_RT_REFLECT=1` siguen mandando sobre los ajustes; `SAVAGEWIND_TEST_CAM="ex,ey,ez,ax,ay,az"` fija la cámara.
