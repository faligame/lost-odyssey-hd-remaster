# Lost Odyssey HD Remaster

**Lost Odyssey, nativo en PC. Hasta 4K, a 60 fps y sin los cierres por los que se le conoce.**

*[Read this in English](README.md)*

![Lost Odyssey HD Remaster](media/hero.png)

> **Estado:** en desarrollo · jugable · código aún no publicado · sin descargas
> Este repositorio es una ventana al progreso, no una release. Ver las [preguntas frecuentes](docs/faq.md).

Lost Odyssey salió en Xbox 360 en 2007 y nunca salió de ahí. Este proyecto reconstruye el código original del juego como un programa de Windows de verdad —**no es un emulador**— y le da después lo que le daría una remasterización.

---

## Qué trae

- **De 720p a 4K, y panorámico.** Eliges la resolución de salida —también Steam Deck (1280×800) y dos modos 21:9— y, por separado, con cuánta nitidez se dibuja el 3D.
- **Una interfaz siempre nítida.** Menús, HUD y diálogos se dibujan a la resolución de tu pantalla cueste lo que cueste el 3D, y quedan justo donde los puso la consola.
- **DLSS.** DLAA, Calidad, Equilibrado, Rendimiento y Ultra rendimiento para el 3D, en Direct3D 12 y en Vulkan.
- **60 fps.**
- **Los cierres conocidos ya no están.** La celda de la prisión, la cinemática tras el primer jefe, el tren congelado. No ocurre ninguno, y sin trucos para esquivarlos.
- **Sin tirones de shaders.** El port lee tus discos y prepara todos los pipelines que el juego va a necesitar antes de jugar.
- **Texto nítido.** Las fuentes del juego redibujadas desde cero a cuatro veces la resolución.
- **Packs de texturas HD.** Se cargan en segundo plano, sin tirones; cambia cualquier textura y recárgala sin salir del juego.
- **Sin batallas aleatorias**, con un interruptor. Los jefes y los combates de la historia siguen.
- **Guardar en cualquier sitio.**
- **Turbo hasta ×8**, en un botón.
- **Cuatro discos, sin cambiarlos.** El juego cambia de disco solo.
- **Seis idiomas** para los textos del juego, a elegir desde el menú.
- **Botones de PlayStation en pantalla**, si ese es el mando que tienes en las manos.
- **Todo desde el menú del propio juego**, como si hubiera venido así.

[Todas las características, explicadas →](docs/features.es.md)

---

## Míralo

### 720p frente a 1080p

| Resolución original de la consola | Este port a 1080p |
|---|---|
| ![720p](media/comparison-720p.png) | ![1080p](media/comparison-1080p.png) |

### DLSS

La misma escena, con el 3D ampliado sin DLSS y con DLSS Calidad.

![Sin DLSS y con DLSS](media/dlss-slider.webp)

A tamaño completo: [sin DLSS](media/dlss-off.png) · [con DLSS](media/dlss-on.png)

### Panorámico

![21:9](media/ultrawide.png)

### Texturas HD

![Texturas originales y pack HD](media/hd-textures-slider.webp)

A tamaño completo: [original](media/hd-textures-before.png) · [pack HD](media/hd-textures-after.png)

### Una interfaz que no se mueve

Al subir la resolución de un juego de consola los menús suelen descolocarse: un cuadro fuera de su panel, una línea que no apunta a nada. Aquí no. Y la interfaz se dibuja a la resolución completa de tu pantalla aunque el 3D vaya a menos.

![Info objetivo con su línea](media/target-info.png)

![Menú de combate](media/battle-menu.png)

### Texto que se lee

La misma línea de diálogo, ampliada. Arriba el original, abajo este port.

![Fuente original arriba, fuente redibujada abajo](media/fonts-comparison-zoom.png)

### Opciones que parecen del propio juego

Pulsa **RB** en la pantalla de Configuración del juego y aparecen cuatro pestañas nuevas, dibujadas con la fuente, los paneles y el cursor del propio juego.

![Pestaña Gráficos dentro de la pantalla de Configuración del juego](media/in-game-settings.png)

![Pestaña Extras](media/extras-tab.png)

### Botones de PlayStation

![Glifos de DualSense](media/dualsense-glyphs.png)

### Listo antes de jugar

En el primer arranque el port lee tus discos y prepara los shaders del juego, en una pantalla hecha con las fuentes y las texturas del propio juego.

![Preparando sombreadores](media/shader-prep.png)

### Pantalla de título

![Pantalla de título](media/title-screen.png)

---

## Cómo va

| | |
|---|---|
| **Jugable** | Sí — sesiones largas, partidas guardadas, logros, cinemáticas |
| **Cierres conocidos** | Arreglados |
| **Renderizadores** | Direct3D 12 y Vulkan |
| **DLSS** | En los dos renderizadores |
| **Discos** | Los cuatro, desde carpetas, imágenes ISO o Games on Demand |
| **Linux** | Previsto, aún sin compilar |

La parte honesta: todos los cierres que conocemos están arreglados, pero nadie ha jugado todavía esta versión desde el primer minuto hasta los créditos. La capa de interfaz nítida es lo más reciente en Vulkan, y algunas escalas 3D (×1,25, ×1,75 y otros cuartos) solo se han probado por encima. En pantallas panorámicas los vídeos aún salen estirados.

No hay descarga. Cuando la haya, hará falta tu propia copia del juego.

---

## ¿Quieres los detalles?

- **[Características en detalle](docs/features.es.md)** — qué hace cada una, y la lista completa de fallos arreglados.
- **[Notas técnicas](docs/technical.md)** — cómo se hizo, para quien quiera hacer lo mismo con otro juego. En inglés.
- **[Registro de avances](docs/progress.md)** — qué cambió y cuándo. En inglés.
- **[Preguntas frecuentes](docs/faq.md)** — dónde está el código, qué hará falta para jugar y más. En inglés.

---

## Créditos

Desarrollado y mantenido por **[FaliGame](https://github.com/FaliGame)**.

Apoyado en el trabajo de otros:

- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** — el SDK de recompilación estática sobre el que está construido el port, y el plugin de GPU Xenos del que sale este fork.
- **[Xenia](https://xenia.jp/)** — el emulador cuya investigación sobre la GPU sostiene prácticamente todo el trabajo gráfico de Xbox 360, este proyecto incluido. Los shaders de compute del plugin se compilan a partir de las fuentes de shaders de Xenia, bajo su licencia BSD.
- **boma** — el conjunto original de parches de Xenia Canary para Lost Odyssey, reimplementados aquí como interruptores en tiempo de ejecución.
- **re:Blue** — la recompilación de Blue Dragon, que enseñó cómo debe quedar un port terminado sobre este SDK.
- **[SMAA](https://github.com/iryoku/smaa)** — de Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro y Diego Gutierrez; usado sin modificar bajo su licencia MIT.
- **[lzokay](https://github.com/jackoalan/lzokay)** — descompresión LZO (MIT), para leer las texturas del juego.
- **[stb](https://github.com/nothings/stb)** — escritura de PNG (dominio público), para el volcado de texturas.
- **[NVIDIA DLSS](https://github.com/NVIDIA/DLSS)** — mediante el NVIDIA RTX SDK, bajo su licencia. NVIDIA y DLSS son marcas de NVIDIA Corporation.

---

## Aviso legal

Este repositorio no contiene **código del juego, ni recursos del juego, ni ejecutables**: solo documentación y capturas.

Lost Odyssey es © Microsoft / Mistwalker / Feelplus. Este es un proyecto de preservación y porteo sin afiliación y sin ánimo de lucro. Aquí nunca se distribuirá el juego: cualquier release futura exigirá aportar una copia propia obtenida legalmente.

La documentación de este repositorio es © su autor. Todos los derechos reservados.
