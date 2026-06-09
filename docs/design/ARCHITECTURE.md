# Architecture Map

> To be designed after Phase 0 research is complete.

---

## Component Inventory (Draft)

| Component | Responsibility | Domain Tag |
|-----------|---------------|-----------|
| Window Manager | Create/destroy/move/resize/z-order windows | [WM] |
| Desktop | Background, icons, taskbar rendering | [WM] |
| Terminal Widget | Text buffer, cursor, scrollback, char rendering | [SHELL] |
| Shell Process | Spawn commands, capture output, forward input | [SHELL] |
| Event Loop | Poll input, dispatch to focused window, timer ticks | [WM] |
| Renderer | Double-buffered compositing, dirty rectangles | [ALLEGRO] |

## Interfaces

(To be defined)

## Data Flow

```
Keyboard/Mouse → Allegro Input → Event Loop → Focused Window → Widget
                                                                  ↓
Timer Tick → Event Loop → All Windows (idle/animate)      Shell Process
                                                                  ↓
                                              Output Buffer → Terminal Widget → Renderer → Screen
```

---

*Last updated: 2026-04-28*
