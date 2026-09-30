# Características en detalle

*[Read this in English](features.md)* · [Volver a la portada](../README.es.md)

Todo lo que hace el port, explicado. Para ver cómo funciona cada pieza por dentro, están las [notas técnicas](technical.md).

---

## Los fallos conocidos ya no están

Quien haya jugado a Lost Odyssey en un emulador conoce la lista: tres sitios donde el juego se cuelga o se cierra, con remedios que pasan de guía en guía: baja aquí a 30 fps, no te saltes esta cinemática, no embistas esa puerta dos veces. **Ninguno ocurre en este port, a 60 fps y sin ningún remedio.**

| Fallo conocido | En este port |
|---|---|
| **Celda de la prisión de Grand Staff** (disco 1) — cierre al hablar con Jansen tras el sueño, o al embestir la puerta de la celda más de una vez | **No ocurre.** Probados los dos disparadores a 60 fps, embistiendo la puerta varias veces con dos personajes y sin saltarse ninguna cinemática. |
| **Cinemática tras el primer jefe** (disco 1) — cierre salvo que se baje el juego a 30 fps | **Arreglado.** Aquí también se cerraba al principio; se encontró la causa y se eliminó. Validado en tres partidas de tres a 60 fps. |
| **Tren congelado** (disco 3) — el juego se congela durante la secuencia del tren | **No ocurre.** La parte del tren se jugó entera en una sesión de 41 minutos sin ningún congelamiento. |

Los problemas propios del port de ese mismo tipo también están arreglados:

| Problema | Estado |
|---|---|
| Cierre tras unos 27 minutos de juego | **Arreglado** |
| Fuga de memoria que acababa con cualquier sesión entre los 4 y los 20 minutos | **Arreglado** — 26 minutos de juego con la memoria afectada plana |
| Cierre al listar partidas copiadas de un emulador | **Arreglado** — las cabeceras que faltan se reconstruyen al arrancar |
| Interfaz descolocada a 1080p (cuadros del tutorial, líneas de objetivo, textos animados) | **Arreglado** — la interfaz es ahora exacta a cualquier resolución |
| Uno o dos minutos compilando shaders en cada arranque | **Arreglado** — se compilan una vez por resolución y se guardan en disco |
| Barras a puntos y líneas finas a 900p y 1620p | **Arreglado** |

Si el port llega a cerrarse, deja un informe que nombra la función original de Xbox 360 en la que estaba. [Cómo se encontraron los dos más difíciles →](technical.md#13-two-crashes-worth-writing-down)

---

## Qué añade sobre la versión de Xbox 360

### Cualquier resolución, con la interfaz exactamente en su sitio

La 360 renderiza Lost Odyssey a 1280×720 porque es lo que cabe en los 10 MB de EDRAM de la consola, contando color y profundidad a la vez.

El port deja que el juego siga creyendo que dibuja a 720p y escala dentro del plugin de GPU todo lo que dibuja: escena, HUD, menús y textos. La escala va en pasos de un cuarto, así que no tiene que ser un número entero: 1080p es ×1,5, no una imagen de 1440p reducida. Pagas los píxeles de la resolución que eliges y nada más.

| Preset | Render interno | Escala |
|---|---|---|
| 720p | 1280×720 | ×1 — modo original de consola |
| 900p | 1600×900 | ×1,25 |
| **1080p** | 1920×1080 | ×1,5 |
| 1440p | 2560×1440 | ×2 |
| 1620p | 2880×1620 | ×2,25 |
| 1800p | 3200×1800 | ×2,5 |
| 4K | 3840×2160 | ×3 |

Sobre el preset: SSAA ×1/×2/×3, antialiasing de post-proceso (FXAA, FXAA extremo o SMAA 1x) y filtro de presentación (bilineal, CAS o FSR).

Como el lienzo del propio juego no cambia nunca, la interfaz es correcta por construcción: los cuadros de diálogo, los paneles del tutorial, las líneas de objetivo y los textos animados quedan justo donde los puso la consola, solo que más nítidos.

Esto sustituyó a un planteamiento anterior. La primera versión hacía que el propio juego renderizase un fotograma de 1920×1080, lo que exigió multiplicar por ocho la EDRAM emulada y reescribir la proyección 2D del juego dibujada a dibujada. Funcionaba en casi todo el juego y nunca llegó a cerrar del todo la interfaz: un cuadro del tutorial que se salía de la pantalla, una línea de objetivo que no apuntaba a nada. Se retiró y el código se eliminó. [Los dos planteamientos, y por qué ganó el segundo →](technical.md#1-rendering-above-720p)

### Arranque rápido

La emulación de Xbox 360 traduce los shaders del juego y le pide al driver gráfico que compile unos dos mil pipelines antes del primer fotograma. Cuando la caché del propio driver no acierta, eso son uno o dos minutos: en cada arranque y cada vez que se cambia de resolución.

El port guarda ahora en disco los pipelines ya compilados, como hacen los juegos modernos de PC. El primer arranque a una resolución sigue compilándolos. A partir de ahí, el juego arranca en unos segundos. [Cómo →](technical.md#9-keeping-compiled-pipelines-on-disk)

| Medido en una RTX 3080 | Primer arranque | Siguientes |
|---|---|---|
| Direct3D 12 | unos 2 min | unos 3 s |
| Vulkan | alrededor de 1 min | unos 7 s |

### SMAA

Subpixel Morphological Antialiasing —la implementación de referencia, sin modificar— en tres pasadas de compute sobre el fotograma final, en los dos renderizadores. Bordes más limpios que con FXAA, sin el emborronado general que deja FXAA. [Cómo encaja →](technical.md#5-smaa-on-the-final-frame)

TAA se estudió y se ha dejado fuera a propósito por ahora: hecho bien necesita jitter de cámara por shader, la escena antes del HUD, profundidad, historial y vectores de movimiento.

### Ajustes dentro del menú del propio juego

Abre la pantalla de **Configuración** del juego y pulsa **RB**. Junto a la página original aparecen cuatro pestañas nuevas —**Gráficos**, **Parches**, **Extras** y **Texturas**— que parecen venir de fábrica, porque se dibujan con la fuente, los paneles de metal y el cursor del propio juego.

Esos recursos se leen en tiempo de ejecución de tu propia copia del juego. Nada del juego forma parte de este proyecto.

Se maneja como la página nativa: arriba y abajo para moverse, izquierda y derecha para cambiar un valor, **LB/RB** para cambiar de pestaña y **B** para volver a las opciones del juego. Lo que se puede aplicar al momento se aplica al momento. Lo que necesita reiniciar se guarda y la página ofrece reiniciar el juego, pulsando A dos veces para que una pulsación accidental nunca te cueste el progreso sin guardar. [Cómo están hechas las pestañas →](technical.md#7-new-menu-pages-that-look-native)

El antiguo panel de **F2** sigue existiendo durante el desarrollo y está de salida.

### Parches del juego, conmutables en marcha

Los parches de la comunidad para Xenia Canary (trabajo original de **boma**) están reimplementados como hooks del recompilador en vez de como parches de bytes, así que cada uno es un interruptor que puedes cambiar mientras juegas:

60 fps · corrección del parpadeo de personajes · desactivar occlusion queries · fix del post-proceso escalado · desactivar profundidad de campo · desactivar motion blur · filtrado anisotrópico 16× · desactivar sombras dinámicas

### Sin batallas aleatorias

Una opción que detiene los combates aleatorios mientras exploras. Los combates de guion —jefes, batallas de la historia— no se tocan.

No modifica ningún dato del juego. El port localizó la única función nativa que cuenta tus pasos hacia el siguiente encuentro y le retiene el contador mientras la opción está activa. [Cómo se encontró →](technical.md#10-finding-the-random-encounter-check)

### Guardar en cualquier sitio

Una opción que habilita **Guardar** en el menú System lejos de los puntos de guardado. Usa el guardado del propio juego —la misma pantalla de ranuras, los mismos ficheros— en vez de fingir un punto de guardado.

Pensada para la exploración. El juego no se diseñó para guardarse a mitad de un evento o de una cinemática, así que mejor evitarlo.

### Cuatro discos, sin cambiarlos a mano

Lost Odyssey ocupa cuatro discos y pide el siguiente según avanza la historia. En la 360 de eso se encarga la consola. Aquí se encarga el port: cuando el juego pide un disco, el port lo busca, lo monta y deja que el juego siga. Sin avisos y sin menús.

Los discos se reconocen por la cabecera de su propio ejecutable («disco N de 4»), así que los nombres de ficheros y carpetas dan igual. La estructura prevista es una carpeta por disco junto al ejecutable:

```
Lost Odyssey\
├── lostodyssey.exe
└── data\
    ├── disc1\    default.xex, LO.fpi, xenon_*.fpd ...
    ├── disc2\
    ├── disc3\
    └── disc4\
```

Las carpetas extraídas son la forma recomendada, pero también valen **imágenes ISO** y paquetes **Games on Demand**, leídos donde estén sin extraer ni copiar nada, y también apuntar al `default.xex` de un disco. Si el disco que pide el juego no aparece, el port lo avisa y espera, como haría la consola, para poder añadirlo sin cerrar el juego. [Cómo funciona el cambio de disco →](technical.md#8-four-discs)

### Sustitución de texturas

Sustituye cualquier textura por un PNG y recarga el pack en caliente con **F7**: sin reiniciar y sin reempaquetar.

Las texturas se identifican por un hash de su contenido en vez de por su dirección de memoria, así que un pack sigue funcionando entre sesiones y entre partidas guardadas.

Hay dos formas de conseguir las originales. El port puede volcar las texturas según las va usando el juego. O puede leer directamente los cuatro discos y escribir todas las texturas del juego —más de dieciséis mil, en minuto y medio— sin pisar una sola zona. Los nombres que escribe llevan los mismos hashes que usa el pack, así que se puede preparar un pack entero sin jugar. Las texturas de color, los mapas de normales y los de iluminación van a carpetas separadas. [Cómo →](technical.md#11-every-texture-without-playing-the-game)

### Texto nítido

Las fuentes del juego son atlas de texturas dibujados para una pantalla de 720p, y a más resolución se nota. Reescalarlos los hace más grandes, no más limpios.

En su lugar, las herramientas del port identifican la tipografía con la que se hizo cada atlas, ajustan su tamaño, su grosor y su contorno a los glifos originales y vuelven a dibujar cada glifo desde los trazos vectoriales a cuatro veces la resolución. Las mismas letras, en las mismas posiciones y con las mismas métricas: dibujadas de nuevo en vez de ampliadas. [Cómo →](technical.md#12-rebuilding-the-fonts-instead-of-upscaling-them)

### Prompts de botones de DualSense

El atlas de glifos de botones del juego es una de esas texturas sustituibles, así que los prompts en pantalla pueden mostrar glifos de PlayStation en vez de los de Xbox que traía fijos la versión de 2007. Sin parchear nada y sin una build aparte: va dentro del pack de texturas.

### Turbo

Avance rápido desde ×1,5 hasta ×8, como pulsación mantenida o conmutador, asignable a un botón del mando (**F6** en teclado). Útil en un JRPG de 2007 con pasillos largos y animaciones de combate.
