# VirtualExtent

**VirtualExtent** is an experimental framework for turning **non-VR games** into **true VR experiences** — by injecting a VR rendering pipeline directly into the game at runtime.

---

## Modes of Operation

VirtualExtent has two main modes:

### Full VR 3D Mode
The game is “tricked” into rendering each frame twice — once per eye — allowing the player to experience true stereoscopic depth and 6-DoF head movement.  
VirtualExtent tries to hook into the game’s **rendering pipeline**, intercepting camera transforms, depth buffers, and draw calls to reconstruct a native-feeling VR view.
Exactly where to hook is estimated from heuristics that try to find the games render() function based on the frequency of directX calls and their location in memory.

### Screen Mode
Used for menus, loading screens, or pre-rendered cutscenes where no in-game camera is active.  
In this mode, the captured game frame (or desktop window) is displayed on a **floating virtual screen** inside VR, preserving immersion and continuity even when the game itself isn’t rendering in 3D.

---

## Current status
- The basic screen mode is working.  
- Some games don’t accept the way mouse clicks are injected; this should be solved with a virtual HID driver at some point (possibly [vMulti](https://github.com/djpnewton/vmulti)).  
- The injection system is being developed outside of this project for the moment (oct 2025).
