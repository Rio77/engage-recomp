# Validación de la candidata preparada · 16 de noviembre de 2026

Esta revisión conserva la biblioteca O1 y los guardados del usuario. Las pruebas
usan copias aisladas; el acceso O1 anterior permanece disponible. La candidata
es experimental y los resultados siguientes tienen un alcance concreto.

## Correcciones comprobadas

- Traducción y compilación SPIR-V de TLD.B, TLD4.B y TLD.B multisample. Se probaron
  ramas que convergen al mismo descriptor, descriptores incompatibles y caminos
  sin definición. Los casos ambiguos se rechazan.
- Espera de recursos listos, terminación en otro hilo, fallo y vencimiento real
  del límite de 30 segundos. No se omite silenciosamente el dibujo pendiente.
- Caché Vulkan: aceptación del dispositivo/UUID correctos y rechazo de cabeceras
  truncadas, versiones y dispositivos incompatibles.
- Texturas NVN pendientes, bytes inválidos, copia por valor, finalización del
  original y serial obsoleto. La metadata compartida se conserva hasta el reset;
  la memoria de GPU se libera al finalizar. Los límites del pool se capturan.
- Ritmo 30/60: instrucciones y build ID verificados, registros preservados,
  factores de movimiento/aceleración comprobados y presentación real a 30 Hz.
  Una frecuencia inválida se rechaza. El modo 60 conserva el reloj VI existente.

La suite se recompiló sobre la fuente actual y terminó correctamente. El script
`tools/test_prepared_runtime.sh` permite repetirla dentro de la imagen de
compilación, con el proyecto montado en `/work`.

También pasaron los binarios de regresión existentes de 10.000 notificaciones
de condición, UTF-8/UTF-16LE y metadata/vídeo NV12. Estos tres binarios no se
recompilaron en esta revisión; sus subsistemas no se modificaron.

## Recorrido real del juego

Se completó el final del capítulo desde Manual2: turno enemigo, animaciones,
resultado y guardado en Manual3 de una copia de pruebas. Pasaron las series
6, 7, 16, 18, 19 y 20 después de incorporar la protección de identidad NVN.
Las series 16, 18, 19 y 20 usan el ejecutable final; 16 comprueba 30 FPS reales,
18 parte sin caché y 19/20 reutilizan cachés. Las series 6/7 preceden los últimos
ajustes de límites del pool y presentación a 30 Hz, respectivamente.

En la serie 19 aparecieron las consultas pendientes del mismo recorrido que
antes causaba un acceso inválido. Se aplazaron y el capítulo terminó guardando.
Los registros de estas ejecuciones no muestran fallos fatales de traducción ni
dibujos omitidos. Esto cubre las escenas recorridas y no toda la campaña.

Una prueba de arranque sin caché quedó esperando mientras se generaba código
en paralelo y había presión de memoria. La causa no quedó confirmada. La
repetición aislada bajo GDB llegó al mapa y la serie 18 sin caché completó el
capítulo. Este antecedente se conserva como una limitación de la validación.

## Generación e instalador

Se ejecutó desde el paquete una generación limpia de los tres módulos, las
correcciones del SDK, la emisión de excepciones y los parches de ritmo. Se
compararon 388.898 funciones con la generación utilizada por la biblioteca O1.
La comparación detectó un switch local que podía perderse al regenerar. Se
añadió su recuperación al generador leyendo la tabla del dump y verificando
opcode y destinos; la función resultante se comprobó idéntica a la probada.
Las otras diferencias por archivo eran prototipos repetidos, comentarios y la
ubicación de una función entre dos bloques.

La biblioteca usada para jugar reutiliza la compilación O1 ya terminada y los
dos bloques de ritmo recompilados. Además se enlazó la generación limpia del
instalador: 1.461 bloques reutilizaron objetos O1 tras comprobar que su contenido
coincidía; los bloques 269/270 se recompilaron por cambiar la distribución de una
función. El enlace terminó sin símbolos invitados pendientes. Es una validación
incremental; no una segunda compilación O1 completa desde cero. Se verificaron
las dependencias y los montajes de solo lectura del layout instalable.

El lanzador instalado también arrancó con esa biblioteca recién enlazada y
recargó el progreso posterior al capítulo. La captura revisada muestra la escena
de Clanne y Framme en color, con personajes y entorno visibles y sin errores en
el registro. Se usó otro prefijo de pruebas, independiente de las partidas del
usuario y del acceso O1.

También se inició el lanzador sin una frecuencia impuesta por el entorno,
leyendo `30` de `data/engage-fps`. El registro confirmó 30 Hz y ventanas a
30,0 FPS/34 ms. La preferencia del acceso de pruebas del usuario sigue en su
valor predeterminado de 60.

La distribución excluye ExeFS/RomFS, traducciones C del juego, su biblioteca,
cachés y partidas. La imagen de herramientas se inspeccionó por nombres de
dumps, bibliotecas del juego y claves conocidas, sin coincidencias. El paquete
incluye fuentes, herramientas offline, runtime y licencias. El SHA-256 permite
comprobar que la copia descargada está completa.

## Rendimiento

La comparación final usa Manual2,
la misma cámara inmóvil, configuración, ventana 1280×720, cachés calientes y
límite de potencia de 15 W. Se registra FPS por ventanas, máximo tiempo de
cuadro, tiempos GPU y el sensor `amdgpu/power1_average`. Este sensor no representa
por sí solo el consumo eléctrico completo de la Deck.

| Medición | O1 anterior | Candidata preparada |
| --- | ---: | ---: |
| Mediana de FPS de las ventanas | 46,3 | 46,7 |
| Intervalo de FPS | 44,5–48,0 | 45,7–47,2 |
| Peor cuadro registrado | 26 ms | 26 ms |
| GPU estimada por cuadro presentado | 8,35 ms | 8,62 ms |
| Sensor de potencia, media | 11,88 W | 12,06 W |
| Dibujos omitidos en la medición | 1.213 | 0 |

Se analizaron 13 ventanas de unos dos segundos por variante. La GPU por cuadro
se estima con el tiempo GPU acumulado del registro y la diferencia de cuadros
presentados entre ventanas. Las capturas se hicieron después de medir y muestran
la misma cámara y selección. Es una pareja de pruebas, no una estimación de
toda la campaña: la diferencia de 0,4 FPS y 0,18 W no demuestra una mejora
sostenida de velocidad o consumo. Sí se eliminó la omisión de dibujos en esta
escena. No se bajó la resolución para conseguir el resultado.

La recompilación de CPU convive con la traducción de shaders NVN a Vulkan.
Un recurso aún desconocido necesita preparación en su primer uso. La precarga
de pipelines conocidos se mide aparte de la inicialización de Unity y de la
lectura de recursos. Persisten posibles pausas de carga; no se garantizan
60 FPS constantes durante toda la campaña.

## Comprobaciones que requieren una prueba humana

- Abrir el nuevo acceso desde modo Juego y revisar icono, progreso visible,
  mando, audio e imagen durante una sesión prolongada.
- Comparar desplazamiento real a 30/60 en las zonas de exploración/Hub.
- Revisar escenas y efectos posteriores a los guardados disponibles.
- Continuar investigando el color intermitente de la cinemática de apertura y
  Contenido adicional. El funcionamiento de los DLC no está validado completo.

No se creó ni actualizó una release pública de GitHub con esta candidata.

El acceso separado en Steam y escritorio se llama **Engage Recomp Preparado
(Prueba)**. Las entradas anteriores de Steam se compararon con el respaldo y
permanecieron intactas. Su ejecutable instalado está en
`/home/deck/engage-native-build/prepared-install/current/engage-bringup` y la
biblioteca en el mismo directorio, `engagegame.so`. Las partidas de ese acceso
están en `/home/deck/engage-native-build/prepared-test-data/save`.
