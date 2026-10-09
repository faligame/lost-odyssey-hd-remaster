// Fork (odisea): precreacion de pipelines a partir de los sombreadores del disco.
//
// El exe lee los ShaderCache de los paquetes UE3 que el juego empieza a cargar
// y manda aqui, por cada material usado, pares (VS del disco, PS) con su vertex
// factory y su clase de pasada (odisea_PrewarmSubmit). El VS del disco no es el
// que ve la GPU: el runtime D3D de la 360 rellena sus vfetch con la declaracion
// de vertices. Este modulo aprende esas declaraciones de los VS que el juego ya
// ha usado (mismo microcodigo con los vfetch en blanco) y las aplica:
//   - formato, signo, desplazamiento y stride salen de la declaracion;
//   - el swizzle de destino se remapea segun el tipo (w=1 en float3, x<->z en
//     D3DCOLOR...);
//   - las vfetch de cada grupo se ordenan por (ranura de fetch desc, offset) y
//     se agrupan en prefetch de hasta 8 dwords (la primera full, el resto mini;
//     un grupo de una sola vfetch lleva prefetch 0).
// Los estados de pipeline de cada clase de pasada tambien se aprenden de los
// pipelines ya vistos. Comun a D3D12 y Vulkan; cada backend hace la creacion.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::graphics::odisea::prewarm {

// Lo que llega del exe (copiado). Microcodigo tal cual esta en el disco
// (dwords big-endian en memoria, como los lee LoadShader).
struct Job {
  std::vector<uint32_t> vs_disc;     // VS del disco, vfetch en blanco
  std::vector<uint32_t> vs_header;   // cabecera del bloque 0x102A1101 (big-endian)
  std::vector<uint32_t> ps;          // PS del disco
  uint32_t vertex_factory = 0;
  uint32_t pass_class = 0;
  bool bulk = false;      // de la generacion completa del primer arranque
  bool counted = false;   // ya revisado una vez (para saber cuando acaba)
  bool resolved = false;  // sus pipelines ya estan en cola (barra de progreso)
  bool urgent = false;    // de un paquete que el juego esta cargando ahora: va delante
};

// --- Semilla (prewarm_seed.bin junto al plugin; ver generar_semilla.py) -------
// Declaraciones de vertices y estados por clase aprendidos en otro equipo: sin
// codigo del juego (formatos, desplazamientos, bits de estado). Con ella la
// precreacion funciona desde el primer arranque.
struct SeedElem {
  uint32_t key, a, b, c;
  int8_t remap[4];
};
struct Seed {
  std::unordered_map<uint32_t, std::vector<std::vector<SeedElem>>> decls;  // vertex factory
  struct State {
    uint32_t count;
    std::vector<uint8_t> bytes;
  };
  struct Set {
    uint32_t desc_size = 0;
    std::unordered_map<uint32_t, std::vector<State>> by_class;
  };
  std::unordered_map<uint32_t, Set> sets;  // etiqueta: 'D12R', 'D12T', 'VKFS', 'VKFB'
};
const Seed& GetSeed();
constexpr uint32_t SeedTag(const char (&s)[5]) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
         uint32_t(uint8_t(s[3])) << 24;
}

// --- Pipelines esenciales de la primera ejecucion ------------------------------
// prewarm_essentials.<rov|rtv>.d3d12.xpso junto al plugin: los pipelines (solo
// hashes de sombreadores y estados, sin microcodigo) que el juego usa de verdad
// en una sesion tipica. Sin almacen de sombreadores propio (instalacion nueva),
// el backend le pide al exe el microcodigo del disco 1 (proveedor sincrono),
// fabrica el .xsh/.xpso como si ya se hubiera jugado y la fase de arranque con
// pantalla (no saltable) los crea antes de empezar a jugar.
struct DiscPair {  // mismo formato que OdiseaPrewarmPair del exe
  const uint32_t* vs_ucode;
  uint32_t vs_dword_count;
  const uint32_t* vs_header;
  uint32_t vs_header_dword_count;
  const uint32_t* ps_ucode;
  uint32_t ps_dword_count;
  uint32_t vertex_factory;
  uint32_t pass_class;
};
using EssentialsCb = void (*)(void* ctx, const DiscPair* pairs, uint32_t count);
using EssentialsProviderFn = int (*)(int disc, EssentialsCb cb, void* ctx);
EssentialsProviderFn EssentialsProvider();
// Contenido de prewarm_essentials.<kind>.xpso (kind: "rov.d3d12", "rtv.d3d12"); vacio si no esta.
std::vector<uint8_t> LoadEssentialsFile(const char* kind);

// --- Generacion completa del primer arranque ---------------------------------
// El exe manda todos los pares del disco montado (odisea_PrewarmSubmitBulk) si
// falta la marca de ese disco junto a la biblioteca de pipelines del preset.
// El backend llama a SetLibraryPath al abrir su biblioteca; mientras dura, la
// creacion va a toda maquina y la pantalla nativa del exe muestra el progreso.
void SetLibraryPath(const std::filesystem::path& library_path);
bool BulkActive();
// Cuantos precreados se crean a la vez fuera de la generacion a toda maquina (cvar
// odisea_prewarm_background_threads, minimo 1). Los que pide el juego no cuentan.
uint32_t BackgroundCreationLimit();
// Generacion completa a toda maquina: activa y sin "Jugar ya" (si el jugador la
// suelta, sigue en segundo plano como la precreacion normal).
bool BulkFast();
// Creacion a toda maquina (todos los hilos, prioridad normal): la generacion completa con pantalla.
// Lo de los paquetes que el juego esta cargando (urgentes) va delante en la cola, al ritmo normal.
bool CreationFast();
// Primera vez que se revisa un trabajo de la generacion completa.
void NoteBulkJobDone(Job& job);
// Sus pipelines ya estan en cola (o ya existian): avanza la barra. Los que no
// se pueden precrear (sin declaracion o sin estados) nunca se resuelven y se
// dan por omitidos al terminar.
void NoteBulkJobResolved(Job& job);
// Todos los pares procesados: el backend, con la cola vacia, guarda su
// biblioteca y llama a FinishBulk (marca y fin de la pantalla).
bool BulkAllProcessed();
// Trabajos en cola (sin contar los aplazados).
size_t PendingJobs();
void FinishBulk();

bool Enabled();
bool AsyncNoFrameWait();

// Saca hasta max trabajos (los nuevos y, si cambio lo aprendido, los que
// esperaban declaracion o estados).
void TakeJobs(std::vector<Job>& out, size_t max, uint64_t learn_generation);
// Trabajo que aun no se puede hacer (falta declaracion o estados de su clase).
void Defer(Job&& job);

// Huella del microcodigo con los vfetch en blanco: a partir de los dwords
// big-endian en memoria (disco) o de los valores ya en orden nativo.
uint64_t BlankedHash(const uint32_t* be_dwords, size_t count);
uint64_t BlankedHashNative(std::vector<uint32_t> native);

class VsBinder {
 public:
  // VS que el juego ya ha usado (Shader::ucode_dwords(), ya en orden nativo).
  void ObserveRuntimeVs(uint64_t hash, const uint32_t* native_dwords, size_t count);
  // VS fabricado por la precreacion: no se aprende de el (no lo ha usado el juego).
  void IgnoreRuntimeVs(uint64_t hash) { runtime_blank_.try_emplace(hash, 0); }
  // VS del disco de un trabajo (registra su vertex factory y su clase).
  void AddDiscVs(const Job& job);
  // Declaraciones de la semilla.
  void AddSeed(const Seed& seed);
  // VS de ejecucion predichos para el trabajo (microcodigo big-endian).
  std::vector<std::vector<uint32_t>> Bind(const Job& job) const;
  // Clases de pasada a las que pertenece un VS ya usado (por su huella).
  const std::unordered_set<uint32_t>* ClassesOfRuntime(uint64_t runtime_hash) const;
  // Sube con cada declaracion nueva.
  uint64_t generation() const { return generation_; }
  // Sube con cada clase de pasada nueva (cambia ClassesOfRuntime).
  uint64_t class_generation() const { return class_generation_; }
  size_t decl_count() const;

 private:
  struct Elem {
    uint32_t a = 0, b = 0, c = 0;  // dwords de la vfetch del juego, sin destino ni prefetch/mini
    int8_t remap[4] = {-1, -1, -1, -1};
  };
  using Decl = std::unordered_map<uint32_t, Elem>;  // clave de uso -> elemento
  struct DiscVs {
    std::vector<uint32_t> ucode;  // valores nativos
    std::vector<uint32_t> fetch_index;
    std::vector<uint32_t> fetch_key;
    std::unordered_set<uint32_t> vertex_factories;
    std::unordered_set<uint32_t> classes;
    bool valid = false;
  };
  void Learn(const DiscVs& disc, const std::vector<uint32_t>& runtime);
  static bool Apply(const DiscVs& disc, const Decl& decl, std::vector<uint32_t>& out);

  std::unordered_map<uint64_t, std::vector<uint32_t>> runtime_by_blank_;  // un VS por huella en blanco
  std::unordered_map<uint64_t, std::vector<std::vector<uint32_t>>> runtime_all_by_blank_;
  std::unordered_map<uint64_t, uint64_t> runtime_blank_;  // huella real -> en blanco
  std::unordered_map<uint64_t, DiscVs> disc_;              // huella en blanco -> VS del disco
  std::unordered_map<uint32_t, std::vector<Decl>> decls_;  // vertex factory -> declaraciones
  std::unordered_set<uint64_t> decl_signatures_;
  uint64_t generation_ = 0;
  uint64_t class_generation_ = 0;
};

// Estados de pipeline por clase de pasada. La plantilla es la descripcion del
// backend con las huellas a cero y las modificaciones sin la mascara de
// interpoladores; se guarda como bytes.
class TemplateStore {
 public:
  void Clear();
  void Add(uint32_t pass_class, const void* data, size_t size, uint32_t count = 1);
  // Los estados con al menos el 25 % de los pipelines de la clase (maximo 2).
  std::vector<const std::vector<uint8_t>*> Pick(uint32_t pass_class) const;
  size_t class_count() const { return by_class_.size(); }

 private:
  struct Counted {
    std::vector<uint8_t> bytes;
    uint32_t count = 0;
  };
  std::unordered_map<uint32_t, std::vector<Counted>> by_class_;
};

// Estadistica (log y odisea_PrewarmStats).
void NoteQueued(uint32_t pipelines);
void NotePrewarmHit();
void MaybeLogStats(size_t in_flight, size_t decls);
// Tirones: llamar una vez por envio (EndSubmission); cada 10 s se registran los
// huecos de mas de 50 y 100 ms entre envios y el peor.
void NoteSubmissionForStutter();

// Prioridad del hilo actual (los backends no incluyen windows.h): baja al minimo
// y devuelve la anterior para restaurarla.
int LowerThreadPriority();
void RestoreThreadPriority(int priority);

}  // namespace rex::graphics::odisea::prewarm
