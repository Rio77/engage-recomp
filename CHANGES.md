# Engage Recomp

## 0.3.0

- Desactiva `g_fnprof` durante la ejecución normal (activándose dinámicamente
  solo bajo depuración/pausa) y reemplaza la búsqueda binaria de saltos
  indirectos en `dispatch.c` por un índice directo de dos niveles O(1) junto
  con caché TLS ampliada a 32768 entradas.
- Elimina `__attribute__((optnone))` de la generación de código para optimizar
  las 524 funciones que quedaban en `-O0`, divide funciones `all_labels` y
  `LARGE_EXTENTS` en partes pequeñas para evitar explosiones CFG en LLVM, y
  agrega `-g0` en `engage_build.py` y `Makefile` (reduciendo `engagegame.so`
  de 1.95 GiB a 406 MiB sin secciones DWARF innecesarias).
- Ejecuta el envío y grabación de Vulkan en un hilo dedicado asíncrono
  (`ENGAGE_VK_ASYNC_SUBMIT`, con respaldo por variable de entorno), desbloqueando
  paralelismo entre la CPU invitada y el backend gráfico.
- Reparte los hilos del kernel emulado entre los núcleos lógicos 0–2 de Horizon
  OS (`GetCurrentProcessorNumber` / `SetThreadCoreMask`) e implementa caché en
  hilo del TID en diagnóstico NVN.
- Cachea la disposición de RomFS en `cache/romfs-layout.bin` validada por marca
  de tiempo e `.install-id` (eliminando la pausa inicial de ~8.4 s al recorrer
  206,874 archivos) y libera `g_base_lock` durante `pread()` en `fs.c`.
- Acelera el instalador y la generación de código usando los 8 hilos del CPU
  en `zig cc`, `scan()`/`emit_function()` paralelos vía `fork` en `gen.py`,
  recorrido único + copia paralela de RomFS con `os.scandir`/`FICLONE` y
  `syncfs()` en `install_core.py`.

## 0.2.0

- Corrige el oscurecimiento del pasto durante impactos físicos y mágicos. El
  traductor Maxwell conserva los fragmentos auxiliares para las derivadas en
  secuencias condicionales de descarte, limpieza de salidas y salida. La
  corrección se aplica a los shaders que cumplen ese patrón.
- Conserva una ruta de respaldo para saltos indirectos dentro de cada
  función. Así, una tabla inferida parcialmente durante la generación no
  deja destinos válidos fuera de la biblioteca de una instalación nueva.
- Sincroniza el cierre de la ventana con el renderizado Vulkan para que la
  salida desde el menú del juego vuelva a Steam en Modo Juego.
- Ajusta las barreras entre pases Vulkan y las rutas de imágenes de
  almacenamiento, sincronización NVN y texturas MSAA. La memoria invitada
  admite una ventana de alias de 16 GiB y hasta 4096 regiones registradas.
- Admite decodificación de vídeo NV12 y YUV420P en la ruta de la cinemática
  inicial. La validación visual de cinemáticas se registra por separado.
- Conserva por instalación la caché de Mesa y precalienta pipelines conocidos
  en segundo plano con Graphics Pipeline Library cuando Vulkan lo admite.
  La espera de fotogramas completos se activa mediante una opción de diagnóstico.
- Usa seguimiento de escrituras por páginas con UFFD-WP y validación de la
  caché de vértices en sistemas compatibles. Reduce el trabajo en Vulkan con
  entrada de vértices dinámica y caché de descriptor sets por envío.
- Dirige los despertares del kernel emulado a los hilos que esperan el objeto
  señalado. Amplía las comprobaciones de memoria invitada y del ciclo de vida NVN.
- Mantiene la biblioteca del juego en `-O1` y compila primero las unidades
  mayores (LPT). `ENGAGE_GAMELIB_RESUME=1` reutiliza los objetos que ya se
  generaron en el mismo directorio de compilación.

## 0.1.0

## Actualización del instalador y configuración

- Selección opcional de DLC antes de copiar o compilar, con comprobación de IDs
  de Engage, contenido extraído y duplicados. Admite `ID/romfs` y `ID/contenido`;
  conserva los DLC existentes al reinstalar sin seleccionar otros nuevos.
- Selección persistente de 30/60 FPS desde **F1 → Video → Frame rate**, aplicable
  durante la sesión. Ajusta el ritmo del juego y de presentación; el selector
  externo ya no se genera en instalaciones nuevas.
- El release ofrece el instalador completo (con AppImage incluido), las fuentes
  correspondientes y sus sumas SHA-256.
- Probados los cambios 30→60→30→60 en la cinemática y la importación de ambos DLC
  de prueba. Sigue pendiente validar el ritmo de batalla y el contenido de DLC
  de forma completa; el bloqueo conocido de Contenido adicional no está resuelto.

## Preparación gráfica y ritmo 30/60

Esta revisión conserva la biblioteca optimizada a O1 y el arreglo de los saltos
del final del capítulo. Añade:

- Resolución de descriptores bindless mediante el flujo de control, con rechazo
  de ramas ambiguas. Soporte de la variante 2D multisample de TLD.B.
- Espera de shaders y pipelines necesarios antes de dibujar; los fallos se
  notifican y registran en lugar de omitir silenciosamente geometría.
- Preparación de pipelines conocidos antes de la primera escena, progreso
  visible y reutilización de traducciones/SPIR-V/pipelines compatibles. Los
  recursos aún desconocidos pueden requerir una pausa en su primer uso.
- Comprobación de versión, dispositivo y UUID de la caché Vulkan. La caché de
  traducción ya distingue el contenido y la versión del traductor.
- Preparación anticipada de shaders y pipelines integrada tras la instalación
  (`--prepare-shaders` en el binario nativo y en el lanzador AppImage): extrae
  localmente shaders desde ExeFS/RomFS usando inventarios de metadatos, genera
  `cache/xlat/`, compila `cache/spirv/` y precalienta pipelines en `cache/warm.bin`
  y `cache/vk_pipelines.bin` con journal de recuperación ante fallos del driver,
  progreso en KDialog y cancelación segura sin afectar la instalación del juego.
- Ajuste persistente 30/60 FPS. Cinco puntos verificados de Engage 2.0.0 ajustan
  vSync, velocidad, giro, aceleración y desaceleración del desplazamiento.
  Se desactiva la heurística de ritmo heredada de otro juego.
- Icono junto al ejecutable, además de los accesos.
- Identidad por revisión de carga para shaders gráficos, de cómputo y etapas
  combinadas: reutilizar una dirección no debe recuperar el programa anterior.
  Se conservan las cachés por contenido entre sesiones. También se comprueban
  las actualizaciones pendientes antes de reutilizar una vista de textura.
- Conservación de las listas de argumentos cuando la grabación de comandos
  necesita volver al juego para pedir memoria. Las listas se copian antes de
  esa llamada; los enlaces nulos se inicializan a cero en lugar de conservar
  bytes anteriores de la memoria de comandos.
- Grabación de comandos en memoria propia del host, con copias independientes
  al finalizar y al componer listas. Reciclar memoria de control retira sus
  identificadores, conservando una lista mientras el ejecutor la está leyendo.
  Los descriptores de programa usados por comandos grabados sobreviven a la
  finalización del objeto del juego. Los enlaces explícitamente diferidos
  conservan su lectura al enviar los comandos.
- Recuperación verificada del switch local de formatos de textura al regenerar
  las funciones, para conservar también esa corrección en instalaciones nuevas.
- Identidades verificadas de texturas NVN copiadas por valor. Las consultas de
  niveles a objetos pendientes devuelven cero, permitiendo que Unity difiera su
  uso. Una copia finalizada deja de invalidar la metadata de otras copias. La
  memoria de esa metadata se recupera al reiniciar el juego; la memoria de GPU
  se libera al finalizar la textura. Los límites del pool se conservan aparte
  de la vida del objeto de pool.

Las fuentes ya contienen estas adaptaciones. Los scripts `enable_*` documentan
transformaciones sobre la base previa y no deben volver a ejecutarse sobre esta
fuente parcheada. La distribución AppImage genera el juego mediante
`tools/engage_generate.py` y lo compila mediante `tools/linux/engage_build.py`.

Referencia para los cinco parches de ritmo:
[engage-pc-recomp](https://gitlab.com/engage-pc-recomp/engage-pc-recomp), commit
`5e0ce98f120bd669cb79d3b365d0da1d6d54145b`,
`recomp/patches-engage-v200.txt`, licencia MIT. Las instrucciones originales y
el identificador del módulo se verifican antes de generar esos puntos.

No se modifican los datos originales del juego. La distribución y el árbol de
fuentes excluyen ExeFS, RomFS, bibliotecas recompiladas del juego, traducciones
C generadas, cachés y partidas. El informe de validación distingue las pruebas
automatizadas de las comprobaciones que necesitan modo Juego o revisión humana.
