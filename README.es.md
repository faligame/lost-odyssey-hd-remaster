# Lost Odyssey — Port a PC

**Una versión nativa de PC de Lost Odyssey (Xbox 360), obtenida por recompilación estática del código original del juego. No es un emulador.**

*[Read this in English](README.md)*

![Lost Odyssey corriendo en PC](media/hero.png)

> **Estado:** en desarrollo · jugable · código aún no publicado · sin descargas
> Este repositorio es una ventana al progreso, no una release. Ver las [preguntas frecuentes](docs/faq.md).

---

## Qué es esto

Lost Odyssey salió en 2007 para Xbox 360 y nunca llegó a PC. Este proyecto lo convierte en un ejecutable de PC de verdad.

La recompilación estática traduce el código máquina PowerPC original del juego, función a función, a código fuente x86-64, que después se compila como un binario normal de Windows. No hay emulación de CPU en tiempo de ejecución, ni intérprete, ni JIT: la lógica del juego corre como código nativo en tu procesador. Solo se reimplementa en el anfitrión lo que el juego le pedía a la consola: el flujo de comandos de GPU, el sistema de ficheros, las partidas guardadas, el audio y el mando.

La diferencia práctica es que el juego deja de comportarse como un juego de consola bajo emulación y empieza a comportarse como un juego de PC. Se puede modificar. Se le puede cambiar el renderizador. Su resolución ya no la decide lo que cabía en la memoria de una GPU de 2007.

Construido sobre el SDK de recompilación [ReXGlue](https://github.com/rexglue/rexglue-sdk) (0.10.0), con un fork muy modificado de su plugin de GPU Xenos.

---

## Estado actual

| | |
|---|---|
| **Arranca y se juega** | Sí — menú, partidas guardadas, logros, cinemáticas, sesiones largas |
| **Renderizadores** | Direct3D 12 y Vulkan, los dos en un plugin, elegibles desde el juego |
| **Resoluciones** | Siete presets de 720p a 4K, en los dos renderizadores |
| **Interfaz a cualquier resolución** | Exacta — la misma maquetación que en la consola, dibujada a la resolución elegida |
| **SMAA** | Funcionando en los dos |
| **Pack de texturas** | Funcionando en los dos |
| **Ajustes en el menú del propio juego** | Funcionando |
| **Los cuatro discos** | Cambio de disco automático — carpetas extraídas, ISO o Games on Demand |
| **Arranque** | Segundos, una vez usada cada resolución por primera vez |
| **Linux** | Aún no compilado — el SDK lo soporta, incluido arm64 |
| **Android** | El SDK no lo soporta |

Con honestidad: «jugable» significa que arranca, corre, guarda y aguanta sesiones largas, incluidos cambios de disco reales al disco 2 y al 3. El juego no se ha verificado de principio a fin en los cuatro discos. Los presets intermedios (900p, 1620p, 1800p) son lo más reciente y solo se han probado por encima.

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

Esto sustituyó a un planteamiento anterior. La primera versión hacía que el propio juego renderizase un fotograma de 1920×1080, lo que exigió multiplicar por ocho la EDRAM emulada y reescribir la proyección 2D del juego dibujada a dibujada. Funcionaba en casi todo el juego y nunca llegó a cerrar del todo la interfaz: un cuadro del tutorial que se salía de la pantalla, una línea de objetivo que no apuntaba a nada. Se retiró y el código se eliminó. [Los dos planteamientos, y por qué ganó el segundo →](docs/technical.md#1-rendering-above-720p)

### Arranque rápido

La emulación de Xbox 360 traduce los shaders del juego y le pide al driver gráfico que compile unos dos mil pipelines antes del primer fotograma. Cuando la caché del propio driver no acierta, eso son uno o dos minutos: en cada arranque y cada vez que se cambia de resolución.

El port guarda ahora en disco los pipelines ya compilados, como hacen los juegos modernos de PC. El primer arranque a una resolución sigue compilándolos. A partir de ahí, el juego arranca en unos segundos. [Cómo →](docs/technical.md#9-keeping-compiled-pipelines-on-disk)

| Medido en una RTX 3080 | Primer arranque | Siguientes |
|---|---|---|
| Direct3D 12 | unos 2 min | unos 3 s |
| Vulkan | alrededor de 1 min | unos 7 s |

### SMAA

Subpixel Morphological Antialiasing —la implementación de referencia, sin modificar— en tres pasadas de compute sobre el fotograma final, en los dos renderizadores. Bordes más limpios que con FXAA, sin el emborronado general que deja FXAA. [Cómo encaja →](docs/technical.md#5-smaa-on-the-final-frame)

TAA se estudió y se ha dejado fuera a propósito por ahora: hecho bien necesita jitter de cámara por shader, la escena antes del HUD, profundidad, historial y vectores de movimiento.

### Ajustes dentro del menú del propio juego

Abre la pantalla de **Configuración** del juego y pulsa **RB**. Junto a la página original aparecen cuatro pestañas nuevas —**Gráficos**, **Parches**, **Extras** y **Texturas**— que parecen venir de fábrica, porque se dibujan con la fuente, los paneles de metal y el cursor del propio juego.

Esos recursos se leen en tiempo de ejecución de tu propia copia del juego. Nada del juego forma parte de este proyecto.

Se maneja como la página nativa: arriba y abajo para moverse, izquierda y derecha para cambiar un valor, **LB/RB** para cambiar de pestaña y **B** para volver a las opciones del juego. Lo que se puede aplicar al momento se aplica al momento. Lo que necesita reiniciar se guarda y la página ofrece reiniciar el juego, pulsando A dos veces para que una pulsación accidental nunca te cueste el progreso sin guardar. [Cómo están hechas las pestañas →](docs/technical.md#7-new-menu-pages-that-look-native)

El antiguo panel de **F2** sigue existiendo durante el desarrollo y está de salida.

### Parches del juego, conmutables en marcha

Los parches de la comunidad para Xenia Canary (trabajo original de **boma**) están reimplementados como hooks del recompilador en vez de como parches de bytes, así que cada uno es un interruptor que puedes cambiar mientras juegas:

60 fps · corrección del parpadeo de personajes · desactivar occlusion queries · fix del post-proceso escalado · desactivar profundidad de campo · desactivar motion blur · filtrado anisotrópico 16× · desactivar sombras dinámicas

### Sin batallas aleatorias

Una opción que detiene los combates aleatorios mientras exploras. Los combates de guion —jefes, batallas de la historia— no se tocan.

No modifica ningún dato del juego. El port localizó la única función nativa que cuenta tus pasos hacia el siguiente encuentro y le retiene el contador mientras la opción está activa. [Cómo se encontró →](docs/technical.md#10-finding-the-random-encounter-check)

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

Las carpetas extraídas son la forma recomendada, pero también valen **imágenes ISO** y paquetes **Games on Demand**, leídos donde estén sin extraer ni copiar nada, y también apuntar al `default.xex` de un disco. Si el disco que pide el juego no aparece, el port lo avisa y espera, como haría la consola, para poder añadirlo sin cerrar el juego. [Cómo funciona el cambio de disco →](docs/technical.md#8-four-discs)

### Sustitución de texturas

Sustituye cualquier textura por un PNG y recarga el pack en caliente con **F7**: sin reiniciar y sin reempaquetar.

Las texturas se identifican por un hash de su contenido en vez de por su dirección de memoria, así que un pack sigue funcionando entre sesiones y entre partidas guardadas.

Hay dos formas de conseguir las originales. El port puede volcar las texturas según las va usando el juego. O puede leer directamente los cuatro discos y escribir todas las texturas del juego —más de dieciséis mil, en minuto y medio— sin pisar una sola zona. Los nombres que escribe llevan los mismos hashes que usa el pack, así que se puede preparar un pack entero sin jugar. Las texturas de color, los mapas de normales y los de iluminación van a carpetas separadas. [Cómo →](docs/technical.md#11-every-texture-without-playing-the-game)

### Texto nítido

Las fuentes del juego son atlas de texturas dibujados para una pantalla de 720p, y a más resolución se nota. Reescalarlos los hace más grandes, no más limpios.

En su lugar, las herramientas del port identifican la tipografía con la que se hizo cada atlas, ajustan su tamaño, su grosor y su contorno a los glifos originales y vuelven a dibujar cada glifo desde los trazos vectoriales a cuatro veces la resolución. Las mismas letras, en las mismas posiciones y con las mismas métricas: dibujadas de nuevo en vez de ampliadas. [Cómo →](docs/technical.md#12-rebuilding-the-fonts-instead-of-upscaling-them)

### Prompts de botones de DualSense

El atlas de glifos de botones del juego es una de esas texturas sustituibles, así que los prompts en pantalla pueden mostrar glifos de PlayStation en vez de los de Xbox que traía fijos la versión de 2007. Sin parchear nada y sin una build aparte: va dentro del pack de texturas.

### Turbo

Avance rápido desde ×1,5 hasta ×8, como pulsación mantenida o conmutador, asignable a un botón del mando (**F6** en teclado). Útil en un JRPG de 2007 con pasillos largos y animaciones de combate.

### Estabilidad

Están arreglados tres fallos que por sí solos habrían acabado con cualquier partida:

- Un cierre tras unos 27 minutos de juego, por un fallo de reserva de memoria.
- Una fuga lenta en la limpieza de los hilos terminados, que agotaba una región de memoria al cabo de entre cuatro y veinte minutos.
- Un cierre en la pantalla de carga que sigue a un jefe del principio, causado por una carrera en el sistema de audio mientras un banco de sonido aún se estaba cargando.

Las partidas traídas de un emulador se reparan al arrancar para que el juego pueda listarlas. Y si el port se cierra, deja un informe que nombra la función original de Xbox 360 en la que estaba. [Los dos que más costaron →](docs/technical.md#13-two-crashes-worth-writing-down)

---

## Capturas

### 720p frente a 1080p

La misma partida, la misma cámara, dos presets. Fíjate en la interfaz, no en el escenario: a la izquierda está dibujada a los 1280×720 de la consola y estirada hasta tu pantalla. A la derecha está dibujada a 1920×1080.

| 720p — modo original de consola | 1080p |
|---|---|
| ![720p](media/comparison-720p.png) | ![1080p](media/comparison-1080p.png) |

### Ajustes dentro del juego

La pantalla de Configuración del juego con la pestaña Gráficos del port abierta. La fuente, los paneles de metal cepillado, el cursor y la maquetación son los del propio juego, leídos de sus datos al arrancar; la página es nueva.

![Ajustes dentro de la pantalla de Configuración del juego](media/in-game-settings.png)

### Prompts de botones de DualSense

La pantalla de ajustes del propio juego, con glifos de PlayStation en lugar de los botones de Xbox que traía fijos la versión de 2008.

![Glifos de DualSense](media/dualsense-glyphs.png)

### Pantalla de título

El subtítulo «HD Remaster» no está en el juego original. Es una textura sustituida —el pack de texturas trabajando en lo primero que ves— y sirve además para reconocer de un vistazo qué build estás ejecutando.

![Pantalla de título](media/title-screen.png)

### El panel de F2

El panel de desarrollo que llegó primero. Ahora que los ajustes viven en la pantalla de Configuración del propio juego, está de salida.

![Panel de opciones de F2](media/options-menu.png)

---

## Documentación

- **[Notas técnicas](docs/technical.md)** — renderizar por encima de 720p (el intento de resolución nativa y la escala de render fraccionaria que lo sustituyó), el SMAA, las páginas de menú hechas con los recursos del propio juego, los cuatro discos, la caché de pipelines, la opción de los encuentros, el volcado de texturas, las fuentes y dos cierres que merecía la pena dejar escritos.
- **[Registro de avances](docs/progress.md)** — qué cambió y cuándo.
- **[Preguntas frecuentes](docs/faq.md)** — incluido dónde está el código y por qué, y qué hará falta para jugar.

---

## Créditos

Desarrollado y mantenido por **[FaliGame](https://github.com/FaliGame)**.

Apoyado en el trabajo de otros:

- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** — el SDK de recompilación estática sobre el que está construido el port, y el plugin de GPU Xenos del que sale este fork.
- **[Xenia](https://xenia.jp/)** — el emulador cuya investigación sobre la GPU sostiene prácticamente todo el trabajo gráfico de Xbox 360, este proyecto incluido. Los shaders de compute del plugin se compilan a partir de las fuentes de shaders de Xenia, bajo su licencia BSD.
- **boma** — el conjunto original de parches de Xenia Canary para Lost Odyssey, reimplementados aquí como hooks en tiempo de ejecución.
- **re:Blue** — la recompilación de Blue Dragon, que enseñó cómo debe quedar un port terminado sobre este SDK.
- **[SMAA](https://github.com/iryoku/smaa)** — de Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro y Diego Gutierrez; usado sin modificar bajo su licencia MIT.
- **[lzokay](https://github.com/jackoalan/lzokay)** — descompresión LZO (MIT), para leer las texturas del juego.
- **[stb](https://github.com/nothings/stb)** — escritura de PNG (dominio público), para el volcado de texturas.

---

## Aviso legal

Este repositorio no contiene **código del juego, ni recursos del juego, ni ejecutables**: solo documentación y capturas.

Los ajustes dentro del juego leen la fuente y las texturas del menú de la copia del propio jugador al arrancar; ninguna está guardada en este repositorio ni en el port.

Lost Odyssey es © Microsoft / Mistwalker / Feelplus. Este es un proyecto de preservación y porteo sin afiliación y sin ánimo de lucro. Aquí nunca se distribuirá el juego: cualquier release futura exigirá aportar una copia propia obtenida legalmente.

La documentación de este repositorio es © su autor. Todos los derechos reservados.
