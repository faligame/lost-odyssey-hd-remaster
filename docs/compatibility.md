# Compatibility and test hardware

## Compatible game version

| | |
|---|---|
| **Game** | *Lost Odyssey* for Xbox 360 — the **"USA, Europe" release**, the one that carries all the languages on the same discs: English, Japanese, French, German, Spanish and Italian |
| **Discs** | All four (disc 1 is required) |
| **Title ID** | `4D5307FA` |
| **Disc media IDs** | disc 1 `368DE6DD` · disc 2 `1888BE4E` · disc 3 `6DD59D08` · disc 4 `0C0E80B5` |
| **Disc 1 `default.xex` SHA-256** | `175ae53d109d480a83bebbd186e7b6871f7b03ce80af69ab388db2f747640de3` |
| **Accepted as** | extracted folders, `.iso` images or Games on Demand packages |
| **Not supported yet** | the Asian edition, modified or damaged discs |

The installer checks every disc against these values and tells you if it is another edition. The game language is chosen in the installer and can be changed later with F2.

## Tested on one PC only

Everything in this version was developed and tuned on a single machine:

- Windows 11 Pro
- Intel Core i9-10850K (10 cores / 20 threads), 32 GB RAM
- NVIDIA GeForce RTX 3080 (10 GB)
- Direct3D 12 (main), Vulkan (secondary), DLSS on RTX cards

It has **not** been tested on AMD or Intel graphics, on laptops or handhelds (Steam Deck), on Windows 10, or on Linux. The performance work (pipeline preparation, threading, resolution scales) is based on that PC's specs, so on other hardware you may see different frame rates or problems we have not met yet. Please report them.

---

# Compatibilidad y equipo de pruebas

## Versión del juego compatible

| | |
|---|---|
| **Juego** | *Lost Odyssey* de Xbox 360 — la edición **"USA, Europe"**, la que trae todos los idiomas en los mismos discos: inglés, japonés, francés, alemán, español e italiano |
| **Discos** | Los cuatro (el disco 1 es obligatorio) |
| **Title ID** | `4D5307FA` |
| **Media ID de los discos** | disco 1 `368DE6DD` · disco 2 `1888BE4E` · disco 3 `6DD59D08` · disco 4 `0C0E80B5` |
| **SHA-256 del `default.xex` del disco 1** | `175ae53d109d480a83bebbd186e7b6871f7b03ce80af69ab388db2f747640de3` |
| **Formatos** | carpetas extraídas, imágenes `.iso` o paquetes Games on Demand |
| **Aún no admitido** | la edición asiática, discos modificados o dañados |

El instalador comprueba cada disco con estos valores y avisa si es otra edición. El idioma del juego se elige en el instalador y se cambia después con F2.

## Probado en un solo PC

Todo lo de esta versión se desarrolló y se ajustó en una única máquina:

- Windows 11 Pro
- Intel Core i9-10850K (10 núcleos / 20 hilos), 32 GB de RAM
- NVIDIA GeForce RTX 3080 (10 GB)
- Direct3D 12 (principal), Vulkan (secundario), DLSS en tarjetas RTX

**No** se ha probado con gráficas AMD o Intel, portátiles o consolas de mano (Steam Deck), Windows 10 ni Linux. El trabajo de rendimiento (preparación de pipelines, hilos, escalas de resolución) está basado en las especificaciones de ese PC, así que en otro equipo puedes ver otros fps o fallos que aún no conocemos. Por favor, avísanos.
