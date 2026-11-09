# Jugador externo ("agent player")

Esta rama añade a fheroes2 1.1.13 un tipo de jugador cuyo turno no lo juega la IA del juego, sino un
programa externo (en nuestro caso, Claude) a través de dos archivos. Sirve para jugar partidas
contra un agente que decide qué construir, qué reclutar y adónde mover a sus héroes.

## Cómo se activa

1. Escribe el color del reino que llevará el agente en `~/.local/share/fheroes2/agent/color`
   (`red`, `blue`, `green`, `yellow`, `orange` o `purple`).
2. Empieza una partida en la que ese color sea **ordenador**.

Los demás reinos de ordenador siguen usando la IA normal. Si el archivo no existe o está vacío, el
juego se comporta exactamente igual que el original.

## Protocolo

Todo ocurre en `~/.local/share/fheroes2/agent/`:

| Archivo | Quién lo escribe | Contenido |
| --- | --- | --- |
| `state.json` | el juego | Estado visible del reino: fecha, recursos, ingresos, héroes (estadísticas, ejército, puntos de movimiento), castillos (edificios construidos y construibles con su coste, criaturas disponibles), héroes contratables, objetos visibles con el coste de movimiento desde cada héroe y un mapa ASCII que respeta la niebla de guerra. Incluye `last_results` con el resultado de cada orden anterior y `status` (`waiting_for_commands` u `opponent_turn`). |
| `brief.txt` | el juego | Resumen compacto en texto (~3 KB) de lo mismo: recursos, héroes, castillos (qué se puede construir/reclutar), taberna, enemigos vistos, los 14 objetos más cercanos por héroe y el resultado de las últimas órdenes. Es lo que el agente debería leer cada turno para gastar pocos tokens; `state.json` queda para el detalle y el mapa ASCII. |
| `commands.txt` | el agente | Una orden por línea. El juego lo lee, lo borra, ejecuta las órdenes y vuelve a escribir `state.json`. |
| `seq` | el juego | Contador que aumenta con cada `state.json` nuevo. |
| `log.txt` | el juego | Histórico de todas las órdenes recibidas. |

### Órdenes

```
move <hero_id> <x> <y>                       # mueve el héroe hacia esa casilla (puede provocar combates)
build <castle_x> <castle_y> <building_id>    # construye (ids en "buildable")
recruit <castle_x> <castle_y> <dwelling_id> <cantidad|max>
upgrade <castle_x> <castle_y>                # mejora las tropas que se pueda pagar
hire <castle_x> <castle_y> <1|2>             # contrata uno de los dos héroes de la taberna
take_garrison <castle_x> <castle_y>          # el héroe del castillo se lleva lo mejor de la guarnición
leave_troop <hero_id> <slot>                 # deja una tropa del héroe en la guarnición
auto_castles                                 # la IA del juego construye y recluta en todos los castillos
auto_hero <hero_id>                          # la IA del juego elige objetivo y camina hasta gastar el movimiento
wait                                         # solo refresca el estado
end                                          # termina el turno
```

Las líneas que empiezan por `#` se ignoran.

Conviene empezar cada lote con `seq <n>`, donde `n` es el `seq` del `state.json`/`brief.txt` que se está
respondiendo. Si el número no coincide (por ejemplo, un archivo antiguo que llega tarde), el juego ignora el
lote entero y lo indica en `last_results`. El juego tampoco lee un `commands.txt` a medio escribir: lo toma
cuando termina en salto de línea o cuando lleva un momento sin cambiar.

## Trucos (rama de pruebas)

En el mapa de aventura, con un héroe seleccionado:

- `B`: añade 10 Dragones Negros a su ejército.
- `G`: añade 10 Titanes.

Se apilan con una tropa igual si ya la tiene; si el ejército está lleno, el juego avisa.

## Limitaciones

- Los combates los resuelve la IA táctica del juego; el agente decide cuándo y contra quién luchar.
- Las decisiones menores durante el movimiento (cofre: oro o experiencia, habilidades al subir de
  nivel, etc.) las toma la IA del juego.
- El agente solo ve lo que su reino puede ver: no hay acceso a lo que cubre la niebla.

## Código

- `src/fheroes2/ai/ai_agent.cpp`: volcado del estado e intérprete de órdenes.
- `src/fheroes2/ai/ai_planner.h`: declaración de `AgentKingdomTurn()` e `isAgentControlled()`.
- `src/fheroes2/game/game_startgame.cpp`: punto donde se decide si el turno de un reino de ordenador lo
  juega la IA o el agente.
