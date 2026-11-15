# Prueba 5 — candidato para hardware

La franja superior/inferior usa ahora el HDMA de la línea válida más cercana. Mode 2 calcula su fase fina por línea. El planificador comparte las fases y usa franjas afines certificadas solo en fondos con HDMA denso; conserva bloques altos en los demás. Los bordes del mundo se certifican por fila. El DSP usa divisibilidad exacta sin división variable; la composición CPU mantiene etiquetas y cobertura en bloques de cuatro píxeles.

Se integran arreglos upstream de tipo de objeto, visibilidad interior, créditos/final, audio de puntuación/muerte, materiales de sprites, estado de diálogo y ciclos de paleta EX. Quedan pendientes márgenes de túneles, clasificación de algunos fondos y la cadena completa de bancos SPC.

CIA 94, Title ID 0004000005f58b00. El icono procede de Forge; la pantalla inferior dice Prueba 5. START+SELECT inicia 65 ventanas en 22 escenarios; R selecciona 212 ventanas; START cancela. 52 fixtures GPU incluyen HDMA y bordes fraccionarios. Los dumps añaden PPU presentada, comandos nativos, input efectivo, tiempos separados, mapas de diferencias y motivos de comparación omitida. Los suplementos llevan SHA-256.

Validación local: 65 estados / 780 dibujos, 54 fondos / cero diferencias CPU-plan, 656 estados BG2 aleatorios, audio de 65 estados / 8.320.000 muestras y estado final idénticos. Los rectángulos de esos 54 fondos bajan de 48.762 a 16.880; en 063, de 10.847 a 529. Son cargas de dibujo, no FPS medidos.

La suite general da 52/57: fallan bloom de escritorio, fixtures MSU/shapes y catálogos ausentes, y la simulación extensa EX. Bloom y EX también fallan con el núcleo anterior. No se ha certificado igualdad completa con SNES, resolución del caso 040, cierre HOME físico ni 60 FPS estables en Old 3DS. Son condiciones de aceptación pendientes del hardware.

Copia los diagnósticos de sdmc:/3ds/Starwing/ después del recorrido y comprueba HOME/volver/cerrar por separado. La huella y el manifiesto del paquete identifican la compilación exacta.
