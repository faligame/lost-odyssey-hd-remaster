// lostodyssey - ReXGlue Recompiled Project
//
// Textos del port en los seis idiomas del juego. Ver lo_i18n.h.
//
// Las palabras que el juego ya usa (Config, Si/No) salen de su propia tabla,
// rpgame\localization\RPGame.<idioma> dentro de coalesced.<idioma>, para que
// las pestanas del port digan lo mismo que la pagina nativa de al lado.
#include "lo_i18n.h"

#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

namespace lo {

namespace {

// Columnas en el orden de Lang: ingles, frances, aleman, italiano, espanol, japones.
struct Entry {
  const char* text[kLangCount];
};

// El texto en espanol es la clave (columna 4).
const Entry kEntries[] = {
    // --- Palabras de la propia tabla del juego (RPGame.<idioma>) ---------------
    {{"Config", "Config", "Konfig.", "Configurazione", "Configuración", "Config"}},
    {{"Yes", "Oui", "Ja", "Sì", "Sí", "はい"}},
    {{"No", "Non", "Nein", "No", "No", "いいえ"}},

    // --- Pestanas ------------------------------------------------------------
    {{"Game", "Jeu", "Spiel", "Gioco", "Juego", "ゲーム"}},
    {{"Graphics", "Graphismes", "Grafik", "Grafica", "Gráficos", "グラフィック"}},
    {{"Patches", "Correctifs", "Patches", "Patch", "Parches", "パッチ"}},
    {{"Extras", "Extras", "Extras", "Extra", "Extras", "その他"}},
    {{"Textures", "Textures", "Texturen", "Texture", "Texturas", "テクスチャ"}},
    {{"Help", "Aide", "Hilfe", "Aiuto", "Ayuda", "ヘルプ"}},

    // --- Graficos ------------------------------------------------------------
    {{"Resolution", "Résolution", "Auflösung", "Risoluzione", "Resolución", "解像度"}},
    {{"Image size and interface sharpness. Deck: 1280x800; UW: 21:9. Wider = more 3D at the sides; "
      "the interface is not stretched. Requires a restart.",
      "Taille de l'image et netteté de l'interface. Deck : 1280x800 ; UW : 21:9. Plus large = plus "
      "de 3D sur les côtés ; l'interface n'est pas étirée. Redémarrage requis.",
      "Bildgröße und Schärfe der Oberfläche. Deck: 1280x800; UW: 21:9. Breiter = mehr 3D an den "
      "Seiten; die Oberfläche wird nicht gestreckt. Neustart nötig.",
      "Dimensione dell'immagine e nitidezza dell'interfaccia. Deck: 1280x800; UW: 21:9. Più largo "
      "= più 3D ai lati; l'interfaccia non si allunga. Richiede il riavvio.",
      "Tamaño de la imagen y nitidez de la interfaz. Deck: 1280x800; UW: 21:9. Más ancho = más 3D "
      "a los lados; la interfaz no se estira. Requiere reiniciar.",
      "画像の大きさとインターフェースの鮮明さ。Deck：1280x800、UW：21:9。横長ほど3Dが"
      "左右に広がり、インターフェースは引き伸ばされません。再起動が必要です。"}},
    {{"With DLSS, the 3D scale is chosen by the DLSS mode for the resolution.", "Avec DLSS, l'échelle 3D est choisie par le mode DLSS selon la résolution.", "Mit DLSS wählt der DLSS-Modus die 3D-Skalierung passend zur Auflösung.", "Con DLSS, la scala 3D la sceglie la modalità DLSS in base alla risoluzione.", "Con DLSS, la escala 3D la elige el modo de DLSS según la resolución.", "DLSS使用時は、3DスケールはDLSSモードと解像度で決まります。"}},
    {{"Not used with DLSS: DLSS already smooths the 3D.",
      "Inutile avec DLSS : DLSS lisse déjà la 3D.",
      "Mit DLSS nicht genutzt: DLSS glättet das 3D bereits.",
      "Non usato con DLSS: DLSS ammorbidisce già il 3D.",
      "Con DLSS no se usa: DLSS ya suaviza el 3D.",
      "DLSS使用時は使いません。DLSSがすでに3Dを滑らかにします。"}},
    {{"Quality", "Qualité", "Qualität", "Qualità", "Calidad", "クオリティ"}},
    {{"Balanced", "Équilibré", "Ausgewogen", "Bilanciato", "Equilibrado", "バランス"}},
    {{"Performance", "Performance", "Leistung", "Prestazioni", "Rendimiento", "パフォーマンス"}},
    {{"Ultra", "Ultra", "Ultra", "Ultra", "Ultra", "ウルトラ"}},
    {{"No DLSS: the 3D is upscaled from the chosen 3D scale. Requires a restart.", "Sans DLSS : la 3D est agrandie depuis l'échelle 3D choisie. Redémarrage requis.", "Ohne DLSS: das 3D wird von der gewählten 3D-Skalierung hochskaliert. Neustart nötig.", "Senza DLSS: il 3D viene ingrandito dalla scala 3D scelta. Richiede il riavvio.", "Sin DLSS: el 3D se reescala desde la escala 3D elegida. Requiere reiniciar.", "DLSSなし：3Dは選んだ3Dスケールから拡大されます。再起動が必要です。"}},
    {{"DLAA: the 3D at the output resolution; DLSS only smooths it. The sharpest. Requires a restart.", "DLAA : la 3D à la résolution de sortie ; DLSS ne fait que lisser. Le plus net. Redémarrage requis.", "DLAA: 3D in Ausgabeauflösung; DLSS glättet nur. Am schärfsten. Neustart nötig.", "DLAA: il 3D alla risoluzione di uscita; DLSS si limita ad ammorbidire. Il più nitido. Richiede il riavvio.", "DLAA: el 3D a la resolución de salida; DLSS solo suaviza. Lo más nítido. Requiere reiniciar.", "DLAA：3Dを出力解像度で描き、DLSSは滑らかにするだけ。最も鮮明。再起動が必要です。"}},
    {{"Quality: the 3D at 2/3 of the resolution (4K: 1440p), rebuilt by DLSS. Requires a restart.", "Qualité : la 3D aux 2/3 de la résolution (4K : 1440p), reconstruite par DLSS. Redémarrage requis.", "Qualität: 3D mit 2/3 der Auflösung (4K: 1440p), von DLSS rekonstruiert. Neustart nötig.", "Qualità: il 3D a 2/3 della risoluzione (4K: 1440p), ricostruito da DLSS. Richiede il riavvio.", "Calidad: el 3D a 2/3 de la resolución (4K: 1440p) y DLSS lo reconstruye. Requiere reiniciar.", "クオリティ：3Dを解像度の2/3（4Kなら1440p）で描き、DLSSが再構成。再起動が必要です。"}},
    {{"Balanced: the 3D at 58% of the resolution (4K: 1260p). Never below 720p. Requires a restart.", "Équilibré : la 3D à 58 % de la résolution (4K : 1260p). Jamais sous 720p. Redémarrage requis.", "Ausgewogen: 3D mit 58 % der Auflösung (4K: 1260p). Nie unter 720p. Neustart nötig.", "Bilanciato: il 3D al 58% della risoluzione (4K: 1260p). Mai sotto 720p. Richiede il riavvio.", "Equilibrado: el 3D al 58 % de la resolución (4K: 1260p). Nunca baja de 720p. Requiere reiniciar.", "バランス：3Dを解像度の58%（4Kなら1260p）で描画。720p未満にはなりません。再起動が必要です。"}},
    {{"Performance: the 3D at half the resolution (4K: 1080p). Never below 720p. Requires a restart.", "Performance : la 3D à la moitié de la résolution (4K : 1080p). Jamais sous 720p. Redémarrage requis.", "Leistung: 3D mit halber Auflösung (4K: 1080p). Nie unter 720p. Neustart nötig.", "Prestazioni: il 3D a metà risoluzione (4K: 1080p). Mai sotto 720p. Richiede il riavvio.", "Rendimiento: el 3D a la mitad de la resolución (4K: 1080p). Nunca baja de 720p. Requiere reiniciar.", "パフォーマンス：3Dを解像度の半分（4Kなら1080p）で描画。720p未満にはなりません。再起動が必要です。"}},
    {{"Ultra performance: the 3D at a third (4K: 720p). Never below 720p. Requires a restart.", "Ultra performance : la 3D au tiers (4K : 720p). Jamais sous 720p. Redémarrage requis.", "Ultra-Leistung: 3D mit einem Drittel (4K: 720p). Nie unter 720p. Neustart nötig.", "Ultra prestazioni: il 3D a un terzo (4K: 720p). Mai sotto 720p. Richiede il riavvio.", "Ultra rendimiento: el 3D a un tercio (4K: 720p). Nunca baja de 720p. Requiere reiniciar.", "ウルトラパフォーマンス：3Dを1/3（4Kなら720p）で描画。720p未満にはなりません。再起動が必要です。"}},
    {{"DLSS model", "Modèle DLSS", "DLSS-Modell", "Modello DLSS", "Modelo de DLSS", "DLSSモデル"}},
    {{"Auto", "Auto", "Auto", "Auto", "Auto", "自動"}},
    {{"Auto: the model NVIDIA picks for each mode (K in DLAA and Quality). Instant.", "Auto : le modèle choisi par NVIDIA pour chaque mode (K en DLAA et Qualité). Immédiat.", "Auto: das Modell, das NVIDIA für jeden Modus wählt (K bei DLAA und Qualität). Sofort.", "Auto: il modello che NVIDIA sceglie per ogni modalità (K in DLAA e Qualità). Immediato.", "Auto: el modelo que NVIDIA elige para cada modo (K en DLAA y Calidad). Al instante.", "自動：NVIDIAがモードごとに選ぶモデル（DLAAとクオリティはK）。即時反映。"}},
    {{"J: a little less trailing and a little more flicker than K. Instant.", "J : un peu moins de traînées et un peu plus de scintillement que K. Immédiat.", "J: etwas weniger Schlieren und etwas mehr Flimmern als K. Sofort.", "J: un po' meno scia e un po' più sfarfallio di K. Immediato.", "J: algo menos de estela y algo más de parpadeo que K. Al instante.", "J：Kより残像が少し少なく、ちらつきが少し多い。即時反映。"}},
    {{"K: sharp and stable; NVIDIA's choice for DLAA and Quality. Instant.", "K : net et stable ; le choix de NVIDIA pour DLAA et Qualité. Immédiat.", "K: scharf und stabil; NVIDIAs Wahl für DLAA und Qualität. Sofort.", "K: nitido e stabile; la scelta di NVIDIA per DLAA e Qualità. Immediato.", "K: nítido y estable; el de NVIDIA para DLAA y Calidad. Al instante.", "K：鮮明で安定。DLAAとクオリティでのNVIDIAの標準。即時反映。"}},
    {{"L: new model (DLSS 4.5), meant for Ultra performance. Instant.", "L : nouveau modèle (DLSS 4.5), pensé pour Ultra performance. Immédiat.", "L: neues Modell (DLSS 4.5), für Ultra-Leistung gedacht. Sofort.", "L: nuovo modello (DLSS 4.5), pensato per Ultra prestazioni. Immediato.", "L: modelo nuevo (DLSS 4.5), pensado para Ultra rendimiento. Al instante.", "L：新モデル（DLSS 4.5）、ウルトラパフォーマンス向け。即時反映。"}},
    {{"M: new model (DLSS 4.5), sharper, meant for Performance. Instant.", "M : nouveau modèle (DLSS 4.5), plus net, pensé pour Performance. Immédiat.", "M: neues Modell (DLSS 4.5), schärfer, für Leistung gedacht. Sofort.", "M: nuovo modello (DLSS 4.5), più nitido, pensato per Prestazioni. Immediato.", "M: modelo nuevo (DLSS 4.5), más nítido, pensado para Rendimiento. Al instante.", "M：新モデル（DLSS 4.5）、より鮮明、パフォーマンス向け。即時反映。"}},
    {{"Choose a DLSS mode first.", "Choisissez d'abord un mode DLSS.", "Zuerst einen DLSS-Modus wählen.", "Scegli prima una modalità DLSS.", "Elige antes un modo de DLSS.", "先にDLSSモードを選んでください。"}},
    {{"3D sharpness", "Netteté 3D", "3D-Schärfe", "Nitidezza 3D", "Nitidez 3D", "3Dシャープネス"}},
    {{"Sharpens the 3D (mainly with DLSS) without touching the interface. Instant.", "Accentue la netteté de la 3D (surtout avec DLSS) sans toucher l'interface. Immédiat.", "Schärft das 3D (vor allem mit DLSS), ohne die Oberfläche zu verändern. Sofort.", "Rende più nitido il 3D (soprattutto con DLSS) senza toccare l'interfaccia. Immediato.", "Enfoca el 3D (sobre todo con DLSS) sin tocar la interfaz. Se aplica al instante.", "3Dを鮮明にします（主にDLSS使用時）。インターフェースは変わりません。即時反映。"}},
    {{"Low", "Faible", "Niedrig", "Bassa", "Baja", "弱"}},
    {{"Medium", "Moyenne", "Mittel", "Media", "Media", "中"}},
    {{"High", "Élevée", "Hoch", "Alta", "Alta", "強"}},
    {{"3D scale", "Échelle 3D", "3D-Skalierung", "Scala 3D", "Escala 3D", "3Dスケール"}},
    {{"Original", "Original", "Original", "Originale", "Original", "オリジナル"}},
    {{"Original: the 3D is drawn at 720p, as on Xbox 360. Requires a restart.",
      "Original : la 3D est rendue en 720p, comme sur Xbox 360. Redémarrage requis.",
      "Original: 3D wird in 720p gezeichnet, wie auf der Xbox 360. Neustart nötig.",
      "Originale: il 3D è disegnato a 720p, come su Xbox 360. Richiede il riavvio.",
      "Original: el 3D se dibuja a 720p, como en Xbox 360. Requiere reiniciar.",
      "オリジナル：3DはXbox 360と同じ720pで描画されます。再起動が必要です。"}},
    {{"x1.5: the 3D is drawn at 1080p (1920x1080). Requires a restart.",
      "x1,5 : la 3D est rendue en 1080p (1920x1080). Redémarrage requis.",
      "x1,5: 3D wird in 1080p (1920x1080) gezeichnet. Neustart nötig.",
      "x1,5: il 3D è disegnato a 1080p (1920x1080). Richiede il riavvio.",
      "x1,5: el 3D se dibuja a 1080p (1920x1080). Requiere reiniciar.",
      "x1.5：3Dを1080p（1920x1080）で描画します。再起動が必要です。"}},
    {{"x2: the 3D is drawn at 1440p (2560x1440). Requires a restart.",
      "x2 : la 3D est rendue en 1440p (2560x1440). Redémarrage requis.",
      "x2: 3D wird in 1440p (2560x1440) gezeichnet. Neustart nötig.",
      "x2: il 3D è disegnato a 1440p (2560x1440). Richiede il riavvio.",
      "x2: el 3D se dibuja a 1440p (2560x1440). Requiere reiniciar.",
      "x2：3Dを1440p（2560x1440）で描画します。再起動が必要です。"}},
    {{"x3: the 3D is drawn at 4K (3840x2160). Requires a restart.",
      "x3 : la 3D est rendue en 4K (3840x2160). Redémarrage requis.",
      "x3: 3D wird in 4K (3840x2160) gezeichnet. Neustart nötig.",
      "x3: il 3D è disegnato a 4K (3840x2160). Richiede il riavvio.",
      "x3: el 3D se dibuja a 4K (3840x2160). Requiere reiniciar.",
      "x3：3Dを4K（3840x2160）で描画します。再起動が必要です。"}},
    {{"x4: the 3D is drawn at 5K (5120x2880). Needs a lot of video memory. Requires a restart.",
      "x4 : la 3D est rendue en 5K (5120x2880). Beaucoup de mémoire vidéo. Redémarrage requis.",
      "x4: 3D wird in 5K (5120x2880) gezeichnet. Viel Grafikspeicher. Neustart nötig.",
      "x4: il 3D è disegnato a 5K (5120x2880). Molta memoria video. Richiede il riavvio.",
      "x4: el 3D se dibuja a 5K (5120x2880). Mucha memoria de vídeo. Requiere reiniciar.",
      "x4：3Dを5K（5120x2880）で描画します。ビデオメモリを多く使います。再起動が必要です。"}},
    {{"x5: the 3D is drawn at 6400x3600. Needs a lot of video memory. Requires a restart.",
      "x5 : la 3D est rendue en 6400x3600. Beaucoup de mémoire vidéo. Redémarrage requis.",
      "x5: 3D wird in 6400x3600 gezeichnet. Viel Grafikspeicher. Neustart nötig.",
      "x5: il 3D è disegnato a 6400x3600. Molta memoria video. Richiede il riavvio.",
      "x5: el 3D se dibuja a 6400x3600. Mucha memoria de vídeo. Requiere reiniciar.",
      "x5：3Dを6400x3600で描画します。ビデオメモリを多く使います。再起動が必要です。"}},
    {{"x6: the 3D is drawn at 8K (7680x4320). Needs a lot of video memory. Requires a restart.",
      "x6 : la 3D est rendue en 8K (7680x4320). Beaucoup de mémoire vidéo. Redémarrage requis.",
      "x6: 3D wird in 8K (7680x4320) gezeichnet. Viel Grafikspeicher. Neustart nötig.",
      "x6: il 3D è disegnato a 8K (7680x4320). Molta memoria video. Richiede il riavvio.",
      "x6: el 3D se dibuja a 8K (7680x4320). Mucha memoria de vídeo. Requiere reiniciar.",
      "x6：3Dを8K（7680x4320）で描画します。ビデオメモリを多く使います。再起動が必要です。"}},
    {{"x7: the 3D is drawn at 8960x5040, the maximum. Needs a lot of video memory. Requires a "
      "restart.",
      "x7 : la 3D est rendue en 8960x5040, le maximum. Beaucoup de mémoire vidéo. Redémarrage "
      "requis.",
      "x7: 3D wird in 8960x5040 gezeichnet, das Maximum. Viel Grafikspeicher. Neustart nötig.",
      "x7: il 3D è disegnato a 8960x5040, il massimo. Molta memoria video. Richiede il riavvio.",
      "x7: el 3D se dibuja a 8960x5040, el máximo. Mucha memoria de vídeo. Requiere reiniciar.",
      "x7：3Dを最大の8960x5040で描画します。ビデオメモリを多く使います。再起動が必要です。"}},
    {{"Fullscreen", "Plein écran", "Vollbild", "Schermo intero", "Pantalla completa", "フルスクリーン"}},
    {{"Fullscreen or windowed. Requires a restart.", "Plein écran ou fenêtré. Redémarrage requis.",
      "Vollbild oder Fenster. Neustart nötig.", "Schermo intero o finestra. Richiede il riavvio.",
      "Pantalla completa o ventana. Requiere reiniciar.", "フルスクリーンかウィンドウか。再起動が必要です。"}},
    {{"V-Sync", "Synchro verticale", "V-Sync", "Sincronia verticale", "Sincronización vertical", "垂直同期"}},
    {{"Prevents screen tearing. Requires a restart.", "Évite le déchirement de l'image. Redémarrage requis.",
      "Verhindert Bildrisse. Neustart nötig.", "Evita lo strappo dell'immagine. Richiede il riavvio.",
      "Evita el desgarro de la imagen. Requiere reiniciar.", "画面のティアリングを防ぎます。再起動が必要です。"}},
    {{"Supersampling", "Suréchantillonnage", "Supersampling", "Supercampionamento", "Supermuestreo",
      "スーパーサンプリング"}},
    {{"Renders at 2x or 3x and scales down: smooths edges, textures and shadows. GPU cost x4 or x9.",
      "Rendu en 2x ou 3x puis réduit : lisse bords, textures et ombres. Coût GPU x4 ou x9.",
      "Rendert in 2x oder 3x und verkleinert: glättet Kanten, Texturen und Schatten. GPU-Last x4 oder x9.",
      "Renderizza a 2x o 3x e riduce: ammorbidisce bordi, texture e ombre. Costo GPU x4 o x9.",
      "Renderiza a 2x o 3x y reduce: suaviza bordes, texturas y sombras. Coste de GPU x4 o x9.",
      "2倍または3倍で描画して縮小します。輪郭・テクスチャ・影が滑らかになります。GPU負荷は4倍または9倍。"}},
    {{"Antialiasing", "Antialiasing", "Antialiasing", "Antialiasing", "Antialiasing", "アンチエイリアス"}},
    {{"Post-process edge smoothing; SMAA is the cleanest. Requires a restart.",
      "Lissage des bords en post-traitement ; SMAA est le plus net. Redémarrage requis.",
      "Kantenglättung per Nachbearbeitung; SMAA ist am saubersten. Neustart nötig.",
      "Smussatura dei bordi in post-elaborazione; SMAA è la più pulita. Richiede il riavvio.",
      "Suavizado de bordes en post-proceso; SMAA es el más limpio. Requiere reiniciar.",
      "ポストプロセスで輪郭を滑らかにします。SMAAが最もきれいです。再起動が必要です。"}},
    {{"FXAA extreme", "FXAA extrême", "FXAA extrem", "FXAA estremo", "FXAA extremo", "FXAA（強）"}},
    {{"Scaling filter", "Filtre de mise à l'échelle", "Skalierungsfilter", "Filtro di scalatura",
      "Filtro de escalado", "スケーリングフィルター"}},
    {{"Filter used to fit the image to the window; CAS and FSR add sharpness.",
      "Filtre pour adapter l'image à la fenêtre ; CAS et FSR ajoutent de la netteté.",
      "Filter beim Anpassen des Bildes an das Fenster; CAS und FSR schärfen nach.",
      "Filtro per adattare l'immagine alla finestra; CAS e FSR aggiungono nitidezza.",
      "Filtro al llevar la imagen a la ventana; CAS y FSR añaden nitidez.",
      "画像をウィンドウに合わせる際のフィルター。CASとFSRはシャープさを加えます。"}},
    {{"Bilinear", "Bilinéaire", "Bilinear", "Bilineare", "Bilineal", "バイリニア"}},
    {{"Graphics API", "API graphique", "Grafik-API", "API grafica", "API gráfica", "グラフィックAPI"}},
    {{"Direct3D 12 is the tested path; Vulkan is experimental. Requires a restart.",
      "Direct3D 12 est la voie éprouvée ; Vulkan est expérimental. Redémarrage requis.",
      "Direct3D 12 ist erprobt; Vulkan ist experimentell. Neustart nötig.",
      "Direct3D 12 è la via collaudata; Vulkan è sperimentale. Richiede il riavvio.",
      "Direct3D 12 es la ruta probada; Vulkan es experimental. Requiere reiniciar.",
      "Direct3D 12が検証済みです。Vulkanは実験的です。再起動が必要です。"}},
    {{"16x anisotropic filtering", "Filtrage anisotrope 16x", "16x anisotrope Filterung",
      "Filtro anisotropico 16x", "Filtro anisótropo 16x", "16x異方性フィルタリング"}},
    {{"Sharp textures at oblique angles. Applies instantly.",
      "Textures nettes sous les angles obliques. S'applique immédiatement.",
      "Scharfe Texturen bei schrägen Blickwinkeln. Wirkt sofort.",
      "Texture nitide ad angoli obliqui. Si applica subito.",
      "Texturas nítidas en ángulos oblicuos. Se aplica al instante.",
      "斜めから見たテクスチャを鮮明にします。すぐに反映されます。"}},
    {{"Restart the game", "Redémarrer le jeu", "Spiel neu starten", "Riavvia il gioco", "Reiniciar el juego",
      "ゲームを再起動"}},
    // Pantalla de preparacion de sombreadores del arranque.
    {{"Preparing shaders", "Préparation des shaders", "Shader werden vorbereitet",
      "Preparazione degli shader", "Preparando sombreadores", "シェーダーを準備中"}},
    {{"Translating shaders", "Traduction des shaders", "Shader werden übersetzt",
      "Traduzione degli shader", "Traduciendo sombreadores", "シェーダーを変換中"}},
    {{"Building pipelines", "Création des pipelines", "Pipelines werden erstellt",
      "Creazione delle pipeline", "Creando pipelines", "パイプラインを作成中"}},
    {{"This only happens the first time and after changing the resolution.",
      "Cela n'arrive que la première fois et après un changement de résolution.",
      "Das passiert nur beim ersten Mal und nach einer Änderung der Auflösung.",
      "Succede solo la prima volta e dopo aver cambiato la risoluzione.",
      "Solo ocurre la primera vez y al cambiar la resolución.",
      "初回と解像度を変更したときだけ行われます。"}},
    {{"Press (A), Start or Enter to play now: the rest is prepared while you play.",
      "Appuyez sur (A), Start ou Entrée pour jouer : le reste se prépare pendant la partie.",
      "(A), Start oder Eingabe drücken, um sofort zu spielen: der Rest wird während des Spiels vorbereitet.",
      "Premi (A), Start o Invio per giocare subito: il resto si prepara mentre giochi.",
      "Pulsa (A), Start o Intro para jugar ya: el resto se prepara mientras juegas.",
      "(A)、STARTかEnterですぐにプレイ：残りはプレイ中に準備します。"}},
    // Precarga de sombreadores (eleccion de la primera vez y opcion de Extras).
    {{"%s (disc %d)", "%s (disque %d)", "%s (Disc %d)", "%s (disco %d)", "%s (disco %d)", "%s（ディスク%d）"}},
    {{"Shader precaching", "Préchargement des shaders", "Shader-Vorladen", "Precaricamento degli shader", "Precarga de sombreadores", "シェーダーの事前読み込み"}},
    {{"Precaching shaders prevents stutters and FPS drops while you play.", "Précharger les shaders évite les saccades et les chutes de FPS en jeu.", "Das Vorladen der Shader verhindert Ruckler und FPS-Einbrüche im Spiel.", "Precaricare gli shader evita scatti e cali di FPS durante il gioco.", "Precargar los sombreadores evita tirones y bajones de FPS durante la partida.", "シェーダーを事前に読み込むと、プレイ中のカクつきやFPSの低下を防げます。"}},
    {{"Precache all shaders (1 disc found)", "Précharger tous les shaders (1 disque trouvé)", "Alle Shader vorladen (1 Disc gefunden)", "Precarica tutti gli shader (1 disco trovato)", "Precargar todos los sombreadores (1 disco encontrado)", "すべてのシェーダーを読み込む（ディスク1枚）"}},
    {{"Precache all shaders (%d discs)", "Précharger tous les shaders (%d disques)", "Alle Shader vorladen (%d Discs)", "Precarica tutti gli shader (%d dischi)", "Precargar todos los sombreadores (%d discos)", "すべてのシェーダーを読み込む（ディスク%d枚）"}},
    {{"Precache the first part (disc %d)", "Précharger la première partie (disque %d)", "Den ersten Teil vorladen (Disc %d)", "Precarica la prima parte (disco %d)", "Precargar los de la primera parte (disco %d)", "最初のパートを読み込む（ディスク%d）"}},
    {{"Precache this part (disc %d)", "Précharger cette partie (disque %d)", "Diesen Teil vorladen (Disc %d)", "Precarica questa parte (disco %d)", "Precargar los de esta parte (disco %d)", "このパートを読み込む（ディスク%d）"}},
    {{"All at once, a few minutes per disc. No more waiting afterwards.", "Tout d'un coup, quelques minutes par disque. Plus d'attente ensuite.", "Alles auf einmal, einige Minuten pro Disc. Danach keine Wartezeit mehr.", "Tutto in una volta, qualche minuto per disco. Poi non si aspetta più.", "Todo de una vez, unos minutos por disco. Después no hay que esperar más.", "一度にすべて。ディスク1枚につき数分。その後は待ち時間なし。"}},
    {{"Faster now. The next parts are precached when you reach them.", "Plus rapide maintenant. Les parties suivantes sont préchargées en y arrivant.", "Jetzt schneller. Die nächsten Teile werden beim Erreichen vorgeladen.", "Più veloce ora. Le parti successive si precaricano quando ci arrivi.", "Más rápido ahora. Las partes siguientes se precargan al llegar a ellas.", "今は速い。次のパートはたどり着いたときに読み込みます。"}},
    {{"While precaching you can press (A), Start or Enter to play now: it carries on in the background, although there may be stutters and FPS drops until it finishes.", "Pendant le préchargement, appuyez sur (A), Start ou Entrée pour jouer : il continue en arrière-plan, mais il peut y avoir des saccades et des chutes de FPS jusqu'à la fin.", "Während des Vorladens kannst du (A), Start oder Eingabe drücken, um sofort zu spielen: es läuft im Hintergrund weiter, bis zum Ende kann es aber Ruckler und FPS-Einbrüche geben.", "Durante il precaricamento puoi premere (A), Start o Invio per giocare subito: continua in background, ma possono esserci scatti e cali di FPS finché non termina.", "Durante la precarga puedes pulsar (A), Start o Intro para jugar ya: seguirá en segundo plano, aunque puede haber tirones y bajones hasta que acabe.", "読み込み中に(A)、STARTかEnterですぐにプレイできます。読み込みはバックグラウンドで続きますが、終わるまでカクつきやFPSの低下が起こることがあります。"}},
    {{"Up/down to choose; (A), Start or Enter to accept. It can be changed in Config > Extras.", "Haut/bas pour choisir ; (A), Start ou Entrée pour valider. Modifiable dans Config > Extras.", "Hoch/runter zum Wählen; (A), Start oder Eingabe zum Bestätigen. Änderbar unter Konfig. > Extras.", "Su/giù per scegliere; (A), Start o Invio per confermare. Si può cambiare in Configurazione > Extra.", "Arriba/abajo para elegir; (A), Start o Intro para aceptar. Se puede cambiar en Config > Extras.", "上下で選択、(A)、STARTかEnterで決定。Config > その他 で変更できます。"}},
    {{"Which pipelines are built at once the first time. Prevents stutters and FPS drops.", "Quels pipelines sont créés d'un coup la première fois. Évite saccades et chutes de FPS.", "Welche Pipelines beim ersten Mal auf einmal erstellt werden. Verhindert Ruckler und FPS-Einbrüche.", "Quali pipeline si creano in una volta la prima volta. Evita scatti e cali di FPS.", "Qué pipelines se crean de golpe la primera vez. Evita tirones y bajones de FPS.", "初回にまとめて作成するパイプライン。カクつきやFPSの低下を防ぎます。"}},
    {{"All discs", "Tous les disques", "Alle Discs", "Tutti i dischi", "Todos los discos", "全ディスク"}},
    {{"Current part", "Partie actuelle", "Aktueller Teil", "Parte attuale", "Parte actual", "現在のパート"}},
    {{"All available discs, one after another (in the background if you are already playing).", "Tous les disques disponibles, l'un après l'autre (en arrière-plan si vous jouez déjà).", "Alle verfügbaren Discs nacheinander (im Hintergrund, wenn du schon spielst).", "Tutti i dischi disponibili, uno dopo l'altro (in background se stai già giocando).", "Todos los discos disponibles, uno detrás de otro (en segundo plano si ya estás jugando).", "使えるすべてのディスクを順番に（プレイ中ならバックグラウンドで）。"}},
    {{"Only the current part; the next ones are precached when you reach them.", "Seulement la partie actuelle ; les suivantes sont préchargées en y arrivant.", "Nur der aktuelle Teil; die nächsten werden beim Erreichen vorgeladen.", "Solo la parte attuale; le successive si precaricano quando ci arrivi.", "Solo la parte actual; las siguientes se precargan al llegar a ellas.", "現在のパートのみ。次のパートはたどり着いたときに読み込みます。"}},
    // Escala 3D en cuartos (ayuda calculada).
    {{"With DLSS the 3D is drawn at %dx%d (set by the mode). Moving the scale turns DLSS off.", "Avec le DLSS, la 3D est rendue en %dx%d (fixé par le mode). Bouger l'échelle désactive le DLSS.", "Mit DLSS wird das 3D in %dx%d gezeichnet (vom Modus). Wer die Skalierung ändert, schaltet DLSS aus.", "Con il DLSS il 3D è disegnato a %dx%d (lo decide la modalità). Muovere la scala disattiva il DLSS.", "Con DLSS el 3D va a %dx%d (lo pone el modo). Al mover la escala se quita el DLSS.", "DLSS使用時、3Dは%dx%dで描画（モードで決定）。スケールを動かすとDLSSはオフになります。"}},
    {{"The 3D is drawn at %dx%d and reduced to the output: supersampling, sharper. Requires a restart.", "La 3D est rendue en %dx%d puis réduite : suréchantillonnage, plus net. Redémarrage requis.", "Das 3D wird in %dx%d gezeichnet und verkleinert: Supersampling, schärfer. Neustart nötig.", "Il 3D è disegnato a %dx%d e ridotto all'uscita: supercampionamento, più nitido. Richiede il riavvio.", "El 3D se dibuja a %dx%d y se reduce a la salida: supermuestreo, más nítido. Requiere reiniciar.", "3Dを%dx%dで描画して出力に縮小：スーパーサンプリングでよりくっきり。再起動が必要です。"}},
    {{"The 3D is drawn at %dx%d. Requires a restart.", "La 3D est rendue en %dx%d. Redémarrage requis.", "Das 3D wird in %dx%d gezeichnet. Neustart nötig.", "Il 3D è disegnato a %dx%d. Richiede il riavvio.", "El 3D se dibuja a %dx%d. Requiere reiniciar.", "3Dは%dx%dで描画されます。再起動が必要です。"}},
    // Fundido entre escenas (Parches).
    {{"Scene crossfade", "Fondu entre les scènes", "Überblendung zwischen Szenen", "Dissolvenza tra le scene", "Fundido entre escenas", "シーン間のフェード"}},
    {{"The previous image fades out over the new one when the scene changes. Applies instantly.", "L'image précédente s'efface sur la nouvelle au changement de scène. S'applique immédiatement.", "Das vorherige Bild blendet beim Szenenwechsel über dem neuen aus. Wirkt sofort.", "L'immagine precedente svanisce su quella nuova al cambio di scena. Si applica subito.", "La imagen anterior se desvanece sobre la nueva al cambiar de escena. Se aplica al instante.", "シーンが変わるとき、前の画像が新しい画像の上で消えていきます。すぐに反映されます。"}},
    {{"Applies the changes that need a restart. Save your game first.",
      "Applique les changements qui nécessitent un redémarrage. Sauvegardez avant.",
      "Übernimmt Änderungen, die einen Neustart brauchen. Vorher speichern.",
      "Applica le modifiche che richiedono il riavvio. Salva prima la partita.",
      "Aplica los cambios que requieren reiniciar. Guarda la partida antes.",
      "再起動が必要な変更を反映します。先にセーブしてください。"}},
    {{"Restart now", "Redémarrer", "Jetzt neu starten", "Riavvia ora", "Reiniciar ahora", "今すぐ再起動"}},

    // --- Parches -------------------------------------------------------------
    {{"Unlocks 60 frames per second. Applies instantly.", "Débloque 60 images par seconde. S'applique immédiatement.",
      "Schaltet 60 Bilder pro Sekunde frei. Wirkt sofort.", "Sblocca 60 fotogrammi al secondo. Si applica subito.",
      "Desbloquea 60 fotogramas por segundo. Se aplica al instante.", "60fpsを解放します。すぐに反映されます。"}},
    {{"Flicker fix", "Anti-scintillement", "Flackern beheben", "Correggi sfarfallio", "Corregir parpadeo",
      "ちらつき修正"}},
    {{"Fixes character flickering. Applies instantly.",
      "Corrige le scintillement des personnages. S'applique immédiatement.",
      "Behebt das Flackern der Figuren. Wirkt sofort.", "Corregge lo sfarfallio dei personaggi. Si applica subito.",
      "Corrige el parpadeo de los personajes. Se aplica al instante.",
      "キャラクターのちらつきを修正します。すぐに反映されます。"}},
    {{"No occlusion queries", "Sans occlusion queries", "Ohne Occlusion Queries", "Senza occlusion query",
      "Sin occlusion queries", "オクルージョンクエリ無効"}},
    {{"Prevents disappearing objects and improves performance.",
      "Évite les objets qui disparaissent et améliore les performances.",
      "Verhindert verschwindende Objekte und verbessert die Leistung.",
      "Evita oggetti che scompaiono e migliora le prestazioni.",
      "Evita objetos que desaparecen y mejora el rendimiento.",
      "オブジェクトの消失を防ぎ、パフォーマンスを向上させます。"}},
    {{"Post-process fix", "Correctif post-traitement", "Nachbearbeitungs-Fix", "Correzione post-elaborazione",
      "Arreglo del post-proceso", "ポストプロセス修正"}},
    {{"Fixes post-processing when rendering above 720p.", "Corrige le post-traitement au-delà de 720p.",
      "Korrigiert die Nachbearbeitung oberhalb von 720p.", "Corregge la post-elaborazione sopra i 720p.",
      "Corrige el post-proceso cuando se renderiza por encima de 720p.",
      "720pを超える描画時のポストプロセスを修正します。"}},
    {{"Depth of field", "Profondeur de champ", "Tiefenschärfe", "Profondità di campo", "Profundidad de campo",
      "被写界深度"}},
    {{"Background blur in cutscenes and battles.", "Flou d'arrière-plan dans les scènes et les combats.",
      "Hintergrundunschärfe in Szenen und Kämpfen.", "Sfocatura dello sfondo in scene e battaglie.",
      "Desenfoque del fondo en escenas y combates.", "イベントや戦闘での背景のぼかし。"}},
    {{"Motion blur", "Flou de mouvement", "Bewegungsunschärfe", "Sfocatura di movimento", "Desenfoque de movimiento",
      "モーションブラー"}},
    {{"Blur when the camera moves.", "Flou lors des mouvements de caméra.", "Unschärfe bei Kamerabewegungen.",
      "Sfocatura quando la telecamera si muove.", "Desenfoque al mover la cámara.", "カメラ移動時のぼかし。"}},
    {{"Dynamic shadows", "Ombres dynamiques", "Dynamische Schatten", "Ombre dinamiche", "Sombras dinámicas",
      "動的な影"}},
    {{"Real-time shadows; turning them off improves performance.",
      "Ombres en temps réel ; les désactiver améliore les performances.",
      "Echtzeitschatten; ohne sie läuft das Spiel schneller.",
      "Ombre in tempo reale; disattivarle migliora le prestazioni.",
      "Sombras en tiempo real; quitarlas mejora el rendimiento.",
      "リアルタイムの影。オフにするとパフォーマンスが向上します。"}},

    // --- Extras --------------------------------------------------------------
    {{"Language", "Langue", "Sprache", "Lingua", "Idioma", "言語"}},
    {{"Game language: text, voices and menus. Requires a restart.",
      "Langue du jeu : textes, voix et menus. Redémarrage requis.",
      "Spielsprache: Texte, Stimmen und Menüs. Neustart nötig.",
      "Lingua del gioco: testi, voci e menu. Richiede il riavvio.",
      "Idioma del juego: textos, voces y menús. Requiere reiniciar.",
      "ゲームの言語（文字・音声・メニュー）。再起動が必要です。"}},
    {{"Japanese", "Japonais", "Japanisch", "Giapponese", "Japonés", "日本語"}},
    {{"FPS counter", "Compteur d'IPS", "FPS-Anzeige", "Contatore FPS", "Contador de fps", "FPSカウンター"}},
    {{"Shows the frames per second and how long each frame takes in the top left corner.",
      "Affiche en haut à gauche les images par seconde et la durée de chaque image.",
      "Zeigt oben links die Bilder pro Sekunde und die Dauer jedes Bildes.",
      "Mostra in alto a sinistra i fotogrammi al secondo e quanto dura ciascuno.",
      "Muestra arriba a la izquierda los fotogramas por segundo y lo que tarda cada uno.",
      "左上にフレームレートと1フレームの処理時間を表示します。"}},
    {{"Save anywhere", "Sauvegarde partout", "Überall speichern", "Salva ovunque", "Guardar en cualquier sitio",
      "どこでもセーブ"}},
    {{"Enables Save in the menu away from save points. Use it while exploring.",
      "Permet de sauvegarder depuis le menu hors des points de sauvegarde. À utiliser en exploration.",
      "Erlaubt Speichern im Menü abseits von Speicherpunkten. Beim Erkunden verwenden.",
      "Permette di salvare dal menu lontano dai punti di salvataggio. Usalo esplorando.",
      "Permite Guardar en el menú System fuera de los puntos de guardado. Úsalo explorando.",
      "セーブポイント以外でもメニューからセーブできます。探索中に使ってください。"}},
    {{"Random battles", "Combats aléatoires", "Zufallskämpfe", "Battaglie casuali", "Batallas aleatorias",
      "ランダムエンカウント"}},
    {{"Turn them off to explore without encounters. Story battles still happen.",
      "Désactivez-les pour explorer sans rencontres. Les combats de l'histoire restent.",
      "Abschalten, um ohne Begegnungen zu erkunden. Story-Kämpfe bleiben.",
      "Disattivale per esplorare senza incontri. Le battaglie della storia restano.",
      "Quítalas para explorar sin encuentros. Los combates de la historia siguen saliendo.",
      "オフにすると敵に遭遇せずに探索できます。ストーリーの戦闘は発生します。"}},
    {{"Turbo", "Turbo", "Turbo", "Turbo", "Turbo", "ターボ"}},
    {{"Lets you speed up the game with a controller button or F6.",
      "Permet d'accélérer le jeu avec un bouton de la manette ou F6.",
      "Beschleunigt das Spiel per Controllertaste oder F6.",
      "Permette di accelerare il gioco con un tasto del controller o con F6.",
      "Permite acelerar el juego con un botón del mando o con F6.",
      "コントローラーのボタンかF6でゲームを高速化できます。"}},
    {{"Turbo speed", "Vitesse du turbo", "Turbo-Tempo", "Velocità turbo", "Velocidad del turbo", "ターボ速度"}},
    {{"Speed multiplier while turbo is on.", "Multiplicateur de vitesse quand le turbo est actif.",
      "Tempo-Multiplikator bei aktivem Turbo.", "Moltiplicatore di velocità con il turbo attivo.",
      "Multiplicador de velocidad mientras el turbo está activo.", "ターボ中の速度倍率。"}},
    {{"Turbo button", "Bouton du turbo", "Turbo-Taste", "Tasto turbo", "Botón del turbo", "ターボボタン"}},
    {{"Button that triggers turbo; the game no longer sees it.",
      "Bouton qui active le turbo ; le jeu ne le voit plus.",
      "Taste für den Turbo; das Spiel sieht sie nicht mehr.",
      "Tasto che attiva il turbo; il gioco non lo vede più.",
      "Botón que activa el turbo; el juego deja de verlo.",
      "ターボを起動するボタン。ゲーム側には入力されません。"}},
    {{"Turbo mode", "Mode du turbo", "Turbo-Modus", "Modalità turbo", "Modo del turbo", "ターボモード"}},
    {{"Toggle: press to turn it on or off. Hold: only while held.",
      "Bascule : appuyer pour l'activer ou le couper. Maintien : seulement tant qu'on appuie.",
      "Umschalten: drücken zum Ein- und Ausschalten. Halten: nur solange gedrückt.",
      "Alterna: premi per attivarlo o spegnerlo. Tieni: solo mentre è premuto.",
      "Conmutar: pulsar para activarlo o quitarlo. Mantener: solo mientras se pulsa.",
      "切替：押すたびにオン／オフ。長押し：押している間だけ。"}},
    {{"Toggle", "Bascule", "Umschalten", "Alterna", "Conmutar", "切替"}},
    {{"Hold", "Maintien", "Halten", "Tieni", "Mantener", "長押し"}},

    // --- Texturas ------------------------------------------------------------
    {{"Texture pack", "Pack de textures", "Texturpaket", "Pacchetto texture", "Pack de texturas", "テクスチャパック"}},
    {{"Replaces textures with the PNGs in the textures folder.",
      "Remplace les textures par les PNG du dossier textures.",
      "Ersetzt Texturen durch die PNGs im Ordner textures.",
      "Sostituisce le texture con i PNG della cartella textures.",
      "Sustituye texturas por los PNG de la carpeta textures.",
      "texturesフォルダのPNGでテクスチャを置き換えます。"}},
    {{"Dump textures", "Extraire les textures", "Texturen exportieren", "Esporta texture", "Volcar texturas",
      "テクスチャを書き出す"}},
    {{"Saves every texture the game loads to dump/textures.",
      "Enregistre chaque texture chargée par le jeu dans dump/textures.",
      "Speichert jede geladene Textur in dump/textures.",
      "Salva ogni texture caricata dal gioco in dump/textures.",
      "Guarda cada textura que carga el juego en dump/textures.",
      "ゲームが読み込むテクスチャをdump/texturesに保存します。"}},
    {{"Reload textures", "Recharger les textures", "Texturen neu laden", "Ricarica texture", "Recargar texturas",
      "テクスチャを再読み込み"}},
    {{"Reads the pack folder again without restarting (also F7).",
      "Relit le dossier du pack sans redémarrer (aussi avec F7).",
      "Liest den Paketordner ohne Neustart neu ein (auch mit F7).",
      "Rilegge la cartella del pacchetto senza riavviare (anche con F7).",
      "Vuelve a leer la carpeta del pack sin reiniciar (también con F7).",
      "再起動せずにパックのフォルダを読み直します（F7でも可）。"}},
    {{"Reload", "Recharger", "Neu laden", "Ricarica", "Recargar", "再読み込み"}},

    // --- Avisos de la pagina -------------------------------------------------
    {{"Press A again", "Appuyez encore sur A", "Nochmals A drücken", "Premi di nuovo A", "Pulsa A otra vez",
      "もう一度Aを押す"}},
    {{"No changes waiting for a restart.", "Aucun changement en attente de redémarrage.",
      "Keine Änderungen warten auf einen Neustart.", "Nessuna modifica in attesa di riavvio.",
      "No hay cambios pendientes de reinicio.", "再起動待ちの変更はありません。"}},
    {{"Could not save config.toml.", "Impossible d'enregistrer config.toml.",
      "config.toml konnte nicht gespeichert werden.", "Impossibile salvare config.toml.",
      "No se pudo guardar config.toml.", "config.tomlを保存できませんでした。"}},
    {{"Textures reloaded from the pack folder.", "Textures rechargées depuis le dossier du pack.",
      "Texturen aus dem Paketordner neu geladen.", "Texture ricaricate dalla cartella del pacchetto.",
      "Texturas recargadas desde la carpeta del pack.", "パックのフォルダからテクスチャを再読み込みしました。"}},
    {{"Could not relaunch the game; restart it manually.",
      "Impossible de relancer le jeu ; redémarrez-le manuellement.",
      "Neustart fehlgeschlagen; bitte manuell neu starten.",
      "Impossibile riavviare il gioco; riavvialo a mano.",
      "No se pudo relanzar el juego; reinícialo a mano.",
      "ゲームを再起動できませんでした。手動で再起動してください。"}},

    // --- Discos --------------------------------------------------------------
    {{"Lost Odyssey needs disc {0}.\n\nPut that disc (extracted folder, .iso or GOD package) next to the other "
      "discs or next to the executable and press Retry.\n\nYou can also set its path in config.toml with "
      "lo_discs.",
      "Lost Odyssey a besoin du disque {0}.\n\nPlacez ce disque (dossier extrait, .iso ou paquet GOD) à côté "
      "des autres disques ou de l'exécutable, puis appuyez sur Réessayer.\n\nVous pouvez aussi indiquer son "
      "chemin dans config.toml avec lo_discs.",
      "Lost Odyssey benötigt Disc {0}.\n\nLege diese Disc (entpackter Ordner, .iso oder GOD-Paket) neben die "
      "anderen Discs oder neben die ausführbare Datei und klicke auf Wiederholen.\n\nDu kannst den Pfad auch "
      "in config.toml mit lo_discs angeben.",
      "Lost Odyssey ha bisogno del disco {0}.\n\nMetti quel disco (cartella estratta, .iso o pacchetto GOD) "
      "accanto agli altri dischi o all'eseguibile e premi Riprova.\n\nPuoi anche indicarne il percorso in "
      "config.toml con lo_discs.",
      "Lost Odyssey necesita el disco {0}.\n\nPon ese disco (carpeta extraída, .iso o paquete GOD) junto a los "
      "demás discos o junto al ejecutable y pulsa Reintentar.\n\nTambién puedes indicar su ruta en "
      "config.toml con lo_discs.",
      "Lost Odysseyにはディスク{0}が必要です。\n\nそのディスク（展開したフォルダ、.iso、GODパッケージ）を他の"
      "ディスクか実行ファイルと同じ場所に置いて、［再試行］を押してください。\n\nconfig.tomlのlo_discsで"
      "パスを指定することもできます。"}},
    {{"Lost Odyssey - Disc change", "Lost Odyssey - Changement de disque", "Lost Odyssey - Discwechsel",
      "Lost Odyssey - Cambio disco", "Lost Odyssey - Cambio de disco", "Lost Odyssey - ディスク交換"}},
    {{"Lost Odyssey disc (default.xex, .iso, GOD package)", "Disque de Lost Odyssey (default.xex, .iso, paquet GOD)",
      "Lost-Odyssey-Disc (default.xex, .iso, GOD-Paket)", "Disco di Lost Odyssey (default.xex, .iso, pacchetto GOD)",
      "Disco de Lost Odyssey (default.xex, .iso, paquete GOD)", "Lost Odysseyのディスク（default.xex、.iso、GODパッケージ）"}},
    {{"All files", "Tous les fichiers", "Alle Dateien", "Tutti i file", "Todos los archivos", "すべてのファイル"}},
    {{"Lost Odyssey: choose disc 1 (default.xex, ISO image or GOD package)",
      "Lost Odyssey : choisissez le disque 1 (default.xex, image ISO ou paquet GOD)",
      "Lost Odyssey: Disc 1 auswählen (default.xex, ISO-Abbild oder GOD-Paket)",
      "Lost Odyssey: scegli il disco 1 (default.xex, immagine ISO o pacchetto GOD)",
      "Lost Odyssey: elige el disco 1 (default.xex, imagen ISO o paquete GOD)",
      "Lost Odyssey：ディスク1を選択（default.xex、ISOイメージ、GODパッケージ）"}},
    {{"That is not a Lost Odyssey disc.\n\nChoose its default.xex, the .iso image or the header of the GOD "
      "package (the file with no extension next to the .data folder).",
      "Ce n'est pas un disque de Lost Odyssey.\n\nChoisissez son default.xex, l'image .iso ou l'en-tête du "
      "paquet GOD (le fichier sans extension à côté du dossier .data).",
      "Das ist keine Lost-Odyssey-Disc.\n\nWähle ihre default.xex, das .iso-Abbild oder den Kopf des GOD-Pakets "
      "(die Datei ohne Endung neben dem Ordner .data).",
      "Questo non è un disco di Lost Odyssey.\n\nScegli il suo default.xex, l'immagine .iso o l'intestazione del "
      "pacchetto GOD (il file senza estensione accanto alla cartella .data).",
      "Eso no es un disco de Lost Odyssey.\n\nElige su default.xex, la imagen .iso o la cabecera del paquete GOD "
      "(el fichero sin extensión que está junto a la carpeta .data).",
      "これはLost Odysseyのディスクではありません。\n\ndefault.xex、.isoイメージ、またはGODパッケージのヘッダー"
      "（.dataフォルダの隣にある拡張子のないファイル）を選んでください。"}},

    // --- Instalador, descarga del pack y informe de cierre (v0.0.1; solo ingles y espanol: el resto cae
    // --- al ingles) ---------------------------------------------------------------------------
    {{"That is not a Lost Odyssey disc. Choose its default.xex, the .iso image or the GOD package.", "", "", "", "Eso no es un disco de Lost Odyssey. Elige su default.xex, la imagen .iso o el paquete GOD.", ""}},
    {{"It is another edition of the game (the Asian one is not supported yet). USA/Europe is required.", "", "", "", "Es de otra edición del juego (la asiática aún no está admitida). Hace falta USA/Europa.", ""}},
    {{"The disc files are not the original ones (modified or damaged).", "", "", "", "Los ficheros del disco no son los originales (modificados o dañados).", ""}},
    {{"Choose a Lost Odyssey disc", "", "", "", "Elige un disco de Lost Odyssey", ""}},
    {{"Choose the disc folder (containing its default.xex)", "", "", "", "Elige la carpeta del disco (con su default.xex)", ""}},
    {{"No disc was found. Add them manually.", "", "", "", "No se ha encontrado ningún disco. Añádelos a mano.", ""}},
    {{"The pack manifest has an unknown format.", "", "", "", "El manifiesto del pack tiene un formato desconocido.", ""}},
    {{"The pack manifest is incomplete.", "", "", "", "El manifiesto del pack está incompleto.", ""}},
    {{"The pack manifest does not match its parts.", "", "", "", "El manifiesto del pack no cuadra con sus trozos.", ""}},
    {{"Could not start networking.", "", "", "", "No se pudo iniciar la red.", ""}},
    {{"The HD texture pack has not been published yet.", "", "", "", "El pack de texturas HD aún no está publicado.", ""}},
    {{"Could not read the pack manifest: ", "", "", "", "No se pudo leer el manifiesto del pack: ", ""}},
    {{"There is not enough disk space for the HD texture pack.", "", "", "", "No hay espacio suficiente en el disco para el pack de texturas HD.", ""}},
    {{"Could not reserve the pack's space on disk.", "", "", "", "No se puede reservar el espacio del pack en el disco.", ""}},
    {{"Could not write the pack to disk.", "", "", "", "No se puede escribir el pack en el disco.", ""}},
    {{"The download could not be completed. Try again: it will resume where it left off.", "", "", "", "La descarga no se pudo completar. Vuelve a intentarlo: continuará donde se quedó.", ""}},
    {{"The downloaded pack does not match the original (it was deleted). Try again.", "", "", "", "El pack descargado no coincide con el original (se ha borrado). Vuelve a intentarlo.", ""}},
    {{"Could not save the downloaded pack.", "", "", "", "No se pudo guardar el pack descargado.", ""}},
    {{"Looking for the HD texture pack…", "", "", "", "Buscando el pack de texturas HD…", ""}},
    {{"Downloading HD textures: %d %% (%.1f MB/s)", "", "", "", "Descargando texturas HD: %d %% (%.1f MB/s)", ""}},
    {{"Verifying the HD texture pack: %d %%", "", "", "", "Comprobando el pack de texturas HD: %d %%", ""}},
    {{"HD textures installed.", "", "", "", "Texturas HD instaladas.", ""}},
    {{"Lost Odyssey HD Remaster closed unexpectedly last time.\n\nWould you like to let us know so we can fix it?\n\nGitHub will open with a pre-filled report (version, graphics card and where it failed). Review it and press Submit. No personal or game data is included.", "", "", "", "Lost Odyssey HD Remaster se cerró de forma inesperada la última vez.\n\n¿Quieres avisarnos para que podamos arreglarlo?\n\nSe abrirá GitHub con un informe ya rellenado (versión, tarjeta gráfica y dónde falló). Revísalo y pulsa Enviar. No se incluye ningún dato personal ni del juego.", ""}},
    {{"Lost Odyssey HD Remaster - Crash report", "", "", "", "Lost Odyssey HD Remaster - Informe de cierre", ""}},
    {{"Verified", "", "", "", "Verificado", ""}},
    {{"Not added", "", "", "", "Sin añadir", ""}},
    {{"Other edition", "", "", "", "Otra edición", ""}},
    {{"Modified or damaged", "", "", "", "Modificado o dañado", ""}},
    {{"Not valid", "", "", "", "No válido", ""}},
    {{"Lost Odyssey installation", "", "", "", "Instalación de Lost Odyssey", ""}},
    {{"Step %d of 5", "", "", "", "Paso %d de 5", ""}},
    {{"Welcome. This wizard prepares Lost Odyssey to play on PC with your own discs.", "", "", "", "Bienvenido. Este asistente prepara Lost Odyssey para jugar en PC con tus propios discos.", ""}},
    {{"You need your own copy of the game: disc 1 is required and you can add the other three now or later. Extracted folders, .iso images and GOD packages of the USA/Europe edition are supported. They are installed into the game folder, or read where they are, as you prefer.", "", "", "", "Necesitas tu copia del juego: el disco 1 es obligatorio y los otros tres puedes añadirlos ahora o más tarde. Se admiten carpetas extraídas, imágenes .iso y paquetes GOD de la edición USA/Europa. Se instalan en la carpeta del juego, o se leen donde estén, como prefieras.", ""}},
    {{"Unofficial project, not affiliated with Microsoft, Mistwalker or Square Enix. The game is not included.", "", "", "", "Proyecto no oficial, sin relación con Microsoft, Mistwalker ni Square Enix. El juego no se incluye.", ""}},
    {{"Version 0.0.1: it has many bugs. If something closes, the game will offer to let us know.", "", "", "", "Versión 0.0.1: tiene muchos fallos. Si algo se cierra, el juego te ofrecerá avisarnos.", ""}},
    {{"Choose your discs", "", "", "", "Elige tus discos", ""}},
    {{"Disc %d", "", "", "", "Disco %d", ""}},
    {{"Add file…", "", "", "", "Añadir archivo…", ""}},
    {{"Add folder…", "", "", "", "Añadir carpeta…", ""}},
    {{"Searching…", "", "", "", "Buscando…", ""}},
    {{"Find discs", "", "", "", "Buscar discos", ""}},
    {{"Disc 1 is ready. You can continue; the other discs can be added now or later.", "", "", "", "Disco 1 listo. Puedes seguir; los demás discos se pueden añadir ahora o más tarde.", ""}},
    {{"Disc 1 is required.", "", "", "", "El disco 1 es obligatorio.", ""}},
    {{"High-definition textures", "", "", "", "Texturas en alta definición", ""}},
    {{"The HD pack improves all the game's textures. It is encrypted and only works with your discs. It is about 28 GB.", "", "", "", "El pack HD mejora todas las texturas del juego. Va cifrado y solo funciona con tus discos. Pesa unos 28 GB.", ""}},
    {{"The HD texture pack is already installed.", "", "", "", "El pack de texturas HD ya está instalado.", ""}},
    {{"Download now (continues while you play and resumes if interrupted)", "", "", "", "Descargar ahora (continúa mientras juegas y se reanuda si se corta)", ""}},
    {{"Later (you can download it in Settings > Textures)", "", "", "", "Más tarde (podrás descargarlo en Configuración > Texturas)", ""}},
    {{"Until it is installed, the game uses the original textures.", "", "", "", "Mientras no esté instalado, el juego se ve con las texturas originales.", ""}},
    {{"Prepare shaders", "", "", "", "Preparar sombreadores", ""}},
    {{"This is done once and avoids stutter and texture flicker. A progress bar will appear when the game starts; you can press a button to play right away.", "", "", "", "Se hace una vez y evita tirones y parpadeos de texturas. Al empezar el juego saldrá una barra de progreso; podrás pulsar para jugar ya.", ""}},
    {{"All available discs (%d disc) - recommended", "", "", "", "Todos los discos disponibles (%d disco) - recomendado", ""}},
    {{"All available discs (%d discs) - recommended", "", "", "", "Todos los discos disponibles (%d discos) - recomendado", ""}},
    {{"Only the first part; the next ones when you reach them", "", "", "", "Solo la primera parte; las siguientes, al llegar a ellas", ""}},
    {{"All discs takes a few minutes per disc; you can change this later in Settings > Extras.", "", "", "", "Todos los discos tarda unos minutos por disco; se puede cambiar luego en Configuración > Extras.", ""}},
    {{"All set", "", "", "", "Todo listo", ""}},
    {{"Verified discs: %d of 4.", "", "", "", "Discos verificados: %d de 4.", ""}},
    {{"HD textures: already installed.", "", "", "", "Texturas HD: ya instaladas.", ""}},
    {{"HD textures: will download on start.", "", "", "", "Texturas HD: se descargarán al empezar.", ""}},
    {{"HD textures: later.", "", "", "", "Texturas HD: más tarde.", ""}},
    {{"Shaders: all discs.", "", "", "", "Sombreadores: todos los discos.", ""}},
    {{"Shaders: first part only.", "", "", "", "Sombreadores: solo la primera parte.", ""}},
    {{"Press Play to start. Options can be changed later with F2 or in the game's Settings.", "", "", "", "Pulsa Jugar para empezar. Las opciones se cambian después con F2 o en la Configuración del juego.", ""}},
    {{"Back", "", "", "", "Atrás", ""}},
    {{"Play", "", "", "", "Jugar", ""}},
    {{"Next", "", "", "", "Siguiente", ""}},
    {{"HD texture pack", "", "", "", "Pack de texturas HD", ""}},
    {{"High-definition textures, encrypted and tied to your discs.", "", "", "", "Texturas en alta definición, cifradas y ligadas a tus discos.", ""}},
    {{"Not installed", "", "", "", "No instalado", ""}},
    {{"Downloading", "", "", "", "Descargando", ""}},
    {{"Installed", "", "", "", "Instalado", ""}},
    {{"Download HD textures", "", "", "", "Descargar texturas HD", ""}},
    {{"Downloads the pack (about 28 GB). It continues while you play and resumes if interrupted.", "", "", "", "Descarga el pack (unos 28 GB). Sigue mientras juegas y se reanuda si se corta.", ""}},
    {{"Download now", "", "", "", "Descargar ahora", ""}},
    {{"Downloading the HD texture pack…", "", "", "", "Descargando el pack de texturas HD…", ""}},
    {{"Your system: ", "", "", "", "Tu equipo: ", ""}},
    {{"Warning: your graphics card has little video memory (4 GB or more recommended).", "", "", "", "Aviso: tu tarjeta tiene poca memoria de vídeo (se recomiendan 4 GB o más).", ""}},
    {{"Warning: 16 GB of RAM is recommended.", "", "", "", "Aviso: se recomiendan 16 GB de RAM.", ""}},
    {{"Step %d of 7", "", "", "", "Paso %d de 7", ""}},
    {{"Install the discs", "", "", "", "Instalar los discos", ""}},
    {{"Install into the game folder (recommended)", "", "", "", "Instalar en la carpeta del juego (recomendado)", ""}},
    {{"Use the discs where they are", "", "", "", "Usar los discos donde están", ""}},
    {{"Nothing is copied: the discs (folder, .iso or GOD) must stay in place to play.", "", "", "", "No se copia nada: los discos (carpeta, .iso o GOD) tienen que seguir en su sitio para jugar.", ""}},
    {{"Copies your discs' files into the \"data\" folder next to the game (about 17 GB in total: whatever is identical across discs is stored once in data\\common). Afterwards you don't need the discs and can copy the whole folder: it becomes a regular PC game.", "", "", "", "Copia los ficheros de tus discos a la carpeta \"data\" junto al juego (unos 17 GB en total: lo que es igual en varios discos se guarda una sola vez en data\\common). Después no necesitas los discos y puedes copiar la carpeta entera: queda como un juego de PC.", ""}},
    {{"Install now", "", "", "", "Instalar ahora", ""}},
    {{"Installation complete. You no longer need the discs.", "", "", "", "Instalación completada. Ya no necesitas los discos.", ""}},
    {{"Copying disc %d of %d", "", "", "", "Copiando disco %d de %d", ""}},
    {{"Cancel", "", "", "", "Cancelar", ""}},
    {{"Discs installed in the game folder: %d of 4.", "", "", "", "Discos instalados en la carpeta del juego: %d de 4.", ""}},
    {{"Verified discs (read where they are): %d of 4.", "", "", "", "Discos verificados (se leen donde están): %d de 4.", ""}},
    {{"They are installed into the game folder, or read where they are, as you prefer.", "", "", "", "Se instalan en la carpeta del juego, o se leen donde estén, como prefieras.", ""}},
    {{"Could not read the disc's file list.", "", "", "", "No se pudo leer la lista de ficheros del disco.", ""}},
    {{"There is not enough disk space to install the discs.", "", "", "", "No hay espacio suficiente en el disco para instalar los discos.", ""}},
    {{"Could not open the disc.", "", "", "", "No se pudo abrir el disco.", ""}},
    {{"Copying a disc file failed: ", "", "", "", "Falló la copia de un fichero del disco: ", ""}},
    {{"The installed disc does not match the original. Try again.", "", "", "", "El disco instalado no coincide con el original. Vuelve a intentarlo.", ""}},
    {{"Initial settings", "", "", "", "Ajustes iniciales", ""}},
    {{"Choose language and graphics for the first launch (change them later with F2).", "", "", "", "Elige idioma y gráficos para el primer arranque (se cambian luego con F2).", ""}},
    {{"When you press Play, the game will restart once to apply these settings.", "", "", "", "Al pulsar Jugar, el juego se reiniciará una vez para aplicar estos ajustes.", ""}},
    {{"When you press Play, the game will restart once to apply the chosen settings.", "", "", "", "Al pulsar Jugar, el juego se reiniciará una vez para aplicar los ajustes elegidos.", ""}},
    {{"To begin, point to disc 1 of your copy of the game: an extracted folder, an .iso image or a GOD package (USA/Europe edition).", "", "", "", "Para empezar, indica dónde está el disco 1 de tu copia del juego: una carpeta extraída, una imagen .iso o un paquete GOD (edición USA/Europa).", ""}},
    {{"Disc 1 verified. Preparing the wizard…", "", "", "", "Disco 1 verificado. Preparando el asistente…", ""}},
    {{"Downloading the update", "", "", "", "Descargando la actualización", ""}},
    {{"Preparing the update", "", "", "", "Preparando la actualización", ""}},
    {{"The game could not be updated: ", "", "", "", "No se pudo actualizar el juego: ", ""}},
    {{"\n\nYou can keep playing the current version; it will be tried again later.", "", "", "", "\n\nPuedes seguir jugando con la versión actual; se volverá a intentar más adelante.", ""}},
    {{"A new version of Lost Odyssey HD Remaster is available: ", "", "", "", "Hay una versión nueva de Lost Odyssey HD Remaster: ", ""}},
    {{"you have ", "", "", "", "tienes la ", ""}},
    {{"Yes = update now (it downloads and the game restarts).\nNo = later.\nCancel = don't tell me about this version again.", "", "", "", "Sí = actualizar ahora (se descarga y el juego se reinicia).\nNo = más tarde.\nCancelar = no volver a avisar de esta versión.", ""}},
    {{"You already have the latest version.", "", "", "", "Ya tienes la última versión.", ""}},
};

const std::unordered_map<std::string_view, const Entry*>& Table() {
  static const auto* table = [] {
    auto* t = new std::unordered_map<std::string_view, const Entry*>();
    for (const Entry& e : kEntries) {
      t->emplace(e.text[int(Lang::kSpanish)], &e);
    }
    return t;
  }();
  return *table;
}

}  // namespace

Lang LangFromXLanguage(uint32_t x) {
  switch (x) {
    case 2: return Lang::kJapanese;
    case 3: return Lang::kGerman;
    case 4: return Lang::kFrench;
    case 5: return Lang::kSpanish;
    case 6: return Lang::kItalian;
    default: return Lang::kEnglish;
  }
}

uint32_t XLanguageOf(Lang lang) {
  static constexpr uint32_t kX[kLangCount] = {1, 4, 3, 6, 5, 2};
  return kX[int(lang)];
}

Lang GameLanguage() { return LangFromXLanguage(REXCVAR_GET(user_language)); }

const char* LangFolder(Lang lang) {
  static constexpr const char* kFolders[kLangCount] = {"int", "fra", "deu", "ita", "spa", "jpn"};
  return kFolders[int(lang)];
}

const char* LangNativeName(Lang lang) {
  static constexpr const char* kNames[kLangCount] = {"English", "Français", "Deutsch",
                                                     "Italiano", "Español", "日本語"};
  return kNames[int(lang)];
}

const char* TrIn(Lang lang, const char* es) {
  if (!es) return "";
  const auto& table = Table();
  const auto it = table.find(std::string_view(es));
  if (it == table.end()) return es;
  const char* t = it->second->text[int(lang)];
  return t && *t ? t : it->second->text[int(Lang::kEnglish)];
}

const char* Tr(const char* es) { return TrIn(GameLanguage(), es); }

}  // namespace lo
