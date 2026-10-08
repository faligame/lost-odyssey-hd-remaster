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
| Enemigos congelados, o «explotando» en pinchos, en algunos combates | **Arreglado** — lo causaba uno de los parches de Xenia; ver más abajo |
| Un tirón al entrar en cada zona con un pack HD instalado | **Arreglado** — las texturas HD se cargan ahora en segundo plano |
| Un personaje que llevaba un momento las texturas HD de otro tras cambiar de escena | **Arreglado** |
| Fondos de menú y fundidos corruptos con la capa de interfaz nítida | **Arreglado** |

Si el port llega a cerrarse, deja un informe que nombra la función original de Xbox 360 en la que estaba. Si deja de dibujar fotogramas más de unos segundos, apunta qué estaba haciendo cada hilo. [Cómo se encontraron los dos más difíciles →](technical.md#13-two-crashes-worth-writing-down)

---

## Qué añade sobre la versión de Xbox 360

### Resolución de salida y escala 3D, por separado

La 360 renderiza Lost Odyssey a 1280×720 porque es lo que cabe en los 10 MB de EDRAM de la consola, contando color y profundidad a la vez.

El port deja que el juego siga creyendo que dibuja a 720p y escala su 3D dentro del plugin de GPU. La pestaña **Gráficos** tiene ahora dos deslizadores donde antes había una lista de presets:

- **Resolución** — lo que llega a tu pantalla: 720p, Steam Deck (1280×800), 900p, 1080p, panorámico 1080 (2560×1080), 1440p, 1620p, panorámico 1440 (3440×1440), 1800p o 4K.
- **Escala 3D** — con cuántos píxeles se dibuja el 3D, desde ×1 (los 1280×720 de la consola) hasta ×7, en pasos de un cuarto. El menú la muestra como porcentaje de tu salida. ×1,5 es 1080p, ×2 es 1440p, ×3 es 4K.

Los cuartos importan: 1080p es un ×1,5 de verdad, no una imagen de 1440p reducida. Pagas los píxeles que eliges y nada más. [Cómo funciona una escala fraccionaria →](technical.md#1-rendering-above-720p)

El antiguo ajuste de supersampling desaparece, porque ahora es simplemente una escala 3D por encima de tu salida. El antialiasing de post-proceso (FXAA, FXAA extremo o SMAA 1x) sigue ahí, y se añade **Nitidez 3D** (no, baja, media, alta).

### Una interfaz siempre nítida

Menús, HUD, diálogos y textos ya no se dibujan a la escala del 3D. Van en una capa propia a la resolución completa de salida, compuesta encima del 3D. Juega a 4K con el 3D a 1080p y el texto sigue siendo texto a 4K.

Como el lienzo del propio juego no cambia nunca, la interfaz además es correcta por construcción: los cuadros de diálogo, los paneles del tutorial, las líneas de objetivo y los textos animados quedan justo donde los puso la consola, solo que más nítidos. [Cómo se separa la interfaz del 3D →](technical.md#14-a-sharp-interface-over-a-scaled-3d)

Esto sustituyó a un planteamiento anterior. La primera versión hacía que el propio juego renderizase un fotograma de 1920×1080, lo que exigió multiplicar por ocho la EDRAM emulada y reescribir la proyección 2D del juego dibujada a dibujada. Funcionaba en casi todo el juego y nunca llegó a cerrar del todo la interfaz: un cuadro del tutorial que se salía de la pantalla, una línea de objetivo que no apuntaba a nada. Se retiró y el código se eliminó. [Los dos planteamientos, y por qué ganó el segundo →](technical.md#1-rendering-above-720p)

### DLSS

NVIDIA DLSS para el 3D, en Direct3D 12 y en Vulkan, con tarjetas GeForce RTX: **DLAA**, **Calidad**, **Equilibrado**, **Rendimiento** y **Ultra rendimiento**. No toca la interfaz, que sigue en su capa nítida.

Al elegir un modo se ajusta la escala 3D, y mover la escala a mano apaga DLSS. El 3D nunca baja de los 720p de la consola, así que los modos que caerían en la misma escala que uno mejor salen en gris: a 1080p de salida, Equilibrado, Rendimiento y Ultra serían todos 720p, de modo que solo se ofrecen Calidad y DLAA. Con DLSS activo se apagan SMAA y FXAA.

Un juego de 2007 no le da a DLSS nada de lo que necesita, así que el port lo construye: la profundidad leída de vuelta de la EDRAM emulada, los vectores de movimiento a partir de las matrices de cámara del propio juego y un temblor de cámara de subpíxel aplicado solo a los dibujos 3D. [Cómo →](technical.md#15-dlss-on-an-emulated-gpu)

### Panorámico y Steam Deck

Las dos resoluciones 21:9 y el 16:10 de la Deck ensanchan la vista del 3D en vez de estirarla, y quitan las bandas negras de las cinemáticas del motor. La interfaz se queda en un recuadro 16:9 centrado, como se diseñó.

Pendiente: los vídeos pregrabados se estiran para llenar una pantalla 21:9, y los fundidos a pantalla completa solo cubren el centro.

### Sin tirones de shaders, y un arranque rápido

La emulación de Xbox 360 traduce los shaders del juego según se los encuentra, y le pide al driver gráfico que compile un pipeline la primera vez que se dibuja cada uno. Es el tirón que todo el mundo conoce: un objeto que aparece un instante tarde, un parón la primera vez que se lanza un hechizo.

El port hace ese trabajo antes de jugar. En el primer arranque lee tus discos, encuentra todos los materiales que el juego puede dibujar y prepara sus pipelines en una pantalla hecha con las fuentes y las texturas del propio juego. Eliges si preparar **los cuatro discos** de una vez o **solo la parte que estás jugando**; lo que necesita la zona que se está cargando pasa siempre al principio de la cola, y con **Intro** empiezas a jugar al momento mientras el resto termina en segundo plano. [Cómo →](technical.md#16-preparing-every-shader-from-the-discs)

El resultado se guarda en disco y, desde principios de octubre, es un solo juego para todas las escalas 3D: cambiar la escala ya no obliga a compilar nada otra vez. [Cómo →](technical.md#17-one-pipeline-set-for-every-scale)

| Medido en una RTX 3080 | |
|---|---|
| Preparar los cuatro discos, la primera vez | de unos 12 segundos a cerca de un cuarto de hora, según lo que ya tenga en caché el driver |
| Siguientes arranques, Direct3D 12 | unos 3 s |
| Siguientes arranques, Vulkan | unos 2 s |

### SMAA

Subpixel Morphological Antialiasing —la implementación de referencia, sin modificar— en tres pasadas de compute sobre el fotograma final, en los dos renderizadores. Bordes más limpios que con FXAA, sin el emborronado general que deja FXAA. [Cómo encaja →](technical.md#5-smaa-on-the-final-frame)

TAA se estudió y se ha dejado fuera a propósito por ahora: hecho bien necesita jitter de cámara por shader, la escena antes del HUD, profundidad, historial y vectores de movimiento.

### Ajustes dentro del menú del propio juego

Abre la pantalla de **Configuración** del juego y pulsa **RB**. Junto a la página original aparecen cuatro pestañas nuevas —**Gráficos**, **Parches**, **Extras** y **Texturas**— que parecen venir de fábrica, porque se dibujan con la fuente, los paneles de metal y el cursor del propio juego.

Esos recursos se leen en tiempo de ejecución de tu propia copia del juego. Nada del juego forma parte de este proyecto.

Se maneja como la página nativa: arriba y abajo para moverse, izquierda y derecha para cambiar un valor, **LB/RB** para cambiar de pestaña y **B** para volver a las opciones del juego. Lo que se puede aplicar al momento se aplica al momento. Lo que necesita reiniciar se guarda y la página ofrece reiniciar el juego, pulsando A dos veces para que una pulsación accidental nunca te cueste el progreso sin guardar. [Cómo están hechas las pestañas →](technical.md#7-new-menu-pages-that-look-native)

La pestaña **Extras** reúne además el idioma, la elección de la preparación de shaders y un botón de reinicio. El antiguo panel de **F2** sigue existiendo durante el desarrollo, ahora con contador de fps, y está de salida.

### Parches del juego, conmutables en marcha

Los parches de la comunidad para Xenia Canary (trabajo original de **boma**) están reimplementados como hooks del recompilador en vez de como parches de bytes, así que cada uno es un interruptor que puedes cambiar mientras juegas:

60 fps · desactivar profundidad de campo · desactivar motion blur · desactivar sombras dinámicas · **fundido entre escenas** (nuevo: un corte limpio en vez de un fundido que la capa de interfaz nítida no puede reproducir)

Algunos ya no son interruptores, porque solo tienen una respuesta correcta: el fix del post-proceso escalado y el filtrado anisotrópico 16× van siempre activos. El parche de occlusion queries desaparece por un motivo que vale la pena conocer: congelaba a los enemigos. [Por qué →](technical.md#20-a-patch-that-froze-the-enemies)

### Sombras más suaves

Las sombras dinámicas del juego tenían un borde duro, escalonado, y algo de parpadeo en los personajes. El port les da una penumbra lineal y filtra el mapa de sombras, con un ajuste de suavidad.

El mapa de sombras en sí es de 864×864 fijos en el juego, a la medida de la memoria de la consola. Lo que afila las sombras es subir la escala 3D.

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

Sustituye cualquier textura por un PNG o un DDS (BC1, BC3 o BC7) y recarga el pack en caliente con **F7**: sin reiniciar y sin reempaquetar.

Las texturas HD se cargan en segundo plano: una zona aparece un instante con sus texturas originales y cada una pasa a HD en cuanto está lista, en vez de parar el juego mientras se cargan. [Cómo →](technical.md#19-hd-textures-without-hitches)

Hay en pruebas un pack reescalado de todo el juego —8.245 texturas de personajes, lugares, objetos y combates, unos 32 GB en BC7—. Como todo lo que sale de los datos del juego, no se distribuirá aquí.

Las texturas se identifican por un hash de su contenido en vez de por su dirección de memoria, así que un pack sigue funcionando entre sesiones y entre partidas guardadas.

Hay dos formas de conseguir las originales. El port puede volcar las texturas según las va usando el juego. O puede leer directamente los cuatro discos y escribir todas las texturas del juego —más de dieciséis mil, en minuto y medio— sin pisar una sola zona. Los nombres que escribe llevan los mismos hashes que usa el pack, así que se puede preparar un pack entero sin jugar. Las texturas de color, los mapas de normales y los de iluminación van a carpetas separadas. [Cómo →](technical.md#11-every-texture-without-playing-the-game)

### Texto nítido

Las fuentes del juego son atlas de texturas dibujados para una pantalla de 720p, y a más resolución se nota. Reescalarlos los hace más grandes, no más limpios.

En su lugar, las herramientas del port identifican la tipografía con la que se hizo cada atlas, ajustan su tamaño, su grosor y su contorno a los glifos originales y vuelven a dibujar cada glifo desde los trazos vectoriales a cuatro veces la resolución. Las mismas letras, en las mismas posiciones y con las mismas métricas: dibujadas de nuevo en vez de ampliadas. [Cómo →](technical.md#12-rebuilding-the-fonts-instead-of-upscaling-them)

### Prompts de botones de DualSense

El atlas de glifos de botones del juego es una de esas texturas sustituibles, así que los prompts en pantalla pueden mostrar glifos de PlayStation en vez de los de Xbox que traía fijos la versión de 2007. Sin parchear nada y sin una build aparte: va dentro del pack de texturas.

### Seis idiomas

Los textos del juego en inglés, francés, alemán, italiano, español o japonés, a elegir en la pestaña **Extras**. Los menús y pantallas propios del port siguen el mismo idioma. Las voces se eligen, como en la consola, en la Configuración del propio juego.

### Partidas junto al juego

Las partidas se guardan en una carpeta `SAVE\` normal junto al ejecutable, una carpeta por ranura. Las de versiones anteriores se mueven ahí solas en el primer arranque.

### Turbo

Avance rápido desde ×1,5 hasta ×8, como pulsación mantenida o conmutador, asignable a un botón del mando (**F6** en teclado). Útil en un JRPG de 2007 con pasillos largos y animaciones de combate.
