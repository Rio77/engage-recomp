# Engage Recomp 0.1.0

Port nativo experimental de Fire Emblem Engage 2.0.0 para Linux y Steam Deck.
Necesitás tus propios archivos **ExeFS y RomFS del juego base con la actualización
2.0.0 aplicada**. El instalador no descarga ni incluye el juego.

1. En modo Escritorio, extraé el paquete del release y abrí `Install.desktop`
   o ejecutá `sh Install.sh` desde esa carpeta.
2. Elegí dónde instalarlo (por defecto `~/Games/Engage-Recomp`) y seleccioná
   las carpetas `exefs` y `romfs` de tu copia. Después elegí si querés importar DLC,
   antes de iniciar la compilación. Seleccioná la carpeta con subcarpetas ID/romfs
   o ID/contenido extraído. No se admiten contenedores NSP/XCI/NCA cifrados.
3. Esperá la generación y compilación inicial. Puede tardar horas; el detalle
   queda en `installation.log`. No requiere Podman ni instalar paquetes en SteamOS.
4. Añadí `Engage-Recomp.sh`, dentro de la instalación, como juego externo a Steam.
   **No actives Proton**. Elegí 30 o 60 FPS mientras jugás desde
   **F1 → Video → Frame rate**. La elección se conserva al cerrar.

La AppImage también se puede ejecutar directamente, dándole permiso de ejecución.
El kit usa extracción automática para no depender de FUSE. Se necesitan Python
3.10+, KDialog y soporte Vulkan, ya disponibles en SteamOS.

Las partidas y la configuración quedan en la carpeta elegida. La instalación
prepara el reemplazo aparte y conserva la anterior en `.recovery`. Si falla una
compilación, consultá el registro y repetí el instalador. Si se interrumpe el
cambio final, volver a ejecutarlo recupera la instalación. Los intentos fallidos
quedan en `.installer-work`; con el juego y el instalador cerrados podés guardar
el registro y retirar únicamente esos intentos. No borres `save` ni tu respaldo.

Se prioriza dibujar con los recursos completos, aunque haya una espera inicial.
Los recursos todavía desconocidos pueden producir pausas. Elegir 60 FPS no
garantiza mantenerlos durante toda la campaña. Persisten el color intermitente
en la cinemática inicial y un bloqueo en Contenido adicional; los DLC no están
validados de forma completa. Consultá `VALIDATION.md` y `PREVIOUS-VALIDATION.md`.

Código heredado bajo MIT y dependencias con sus licencias propias. `THIRD_PARTY.md`
y `UPSTREAM.md` detallan autores, referencias y fuentes correspondientes. La
imagen del icono es independiente de la licencia del código. Proyecto no oficial,
sin afiliación con Nintendo, desarrollado con asistencia de IA.

El retrato de victoria puede permanecer oscuro de forma intermitente. Este fallo sigue pendiente en 0.1.0.
