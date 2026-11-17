# Candidata con preparación gráfica y ritmo 30/60

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
- Ajuste persistente 30/60 FPS. Cinco puntos verificados de Engage 2.0.0 ajustan
  vSync, velocidad, giro, aceleración y desaceleración del desplazamiento.
  Se desactiva la heurística de ritmo heredada de otro juego.
- Icono junto al ejecutable, además de los accesos.
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
fuente parcheada. El instalador ejecuta únicamente la generación del juego y
los pasos indicados en `tools/build-game.sh`.

Referencia para los cinco parches de ritmo:
[engage-pc-recomp](https://gitlab.com/engage-pc-recomp/engage-pc-recomp), commit
`5e0ce98f120bd669cb79d3b365d0da1d6d54145b`,
`recomp/patches-engage-v200.txt`, licencia MIT. Las instrucciones originales y
el identificador del módulo se verifican antes de generar esos puntos.

No se modifican los datos originales del juego. La distribución y el árbol de
fuentes excluyen ExeFS, RomFS, bibliotecas recompiladas del juego, traducciones
C generadas, cachés y partidas. El informe de validación distingue las pruebas
automatizadas de las comprobaciones que necesitan modo Juego o revisión humana.
