# kspace

kspace is a small client–server prototype that combines an authoritative game server, a low-level TCP protocol, a raylib-based 3D client, and an in-world terminal backed by a server-side interpreter.

The project is intentionally minimal and explicit. Most systems are implemented “the long way” for clarity and hackability rather than abstraction or performance.

---

## High-level architecture

The project is split into three major parts:

server/   - authoritative simulation + terminal interpreter  
client/   - raylib client (rendering, input, prediction, UI)  
common/   - shared protocol definitions  

The server owns all authoritative state. The client is responsible for presentation, input capture, and light prediction/smoothing.

Networking is done over a simple, line-based TCP protocol.

---

## Server

### Responsibilities

The server is responsible for:

- Accepting a single TCP client
- Maintaining authoritative player state (position, yaw, pitch)
- Applying movement and look input received from the client
- Running a server-side terminal interpreter
- Streaming terminal history and output back to the client

The server does not attempt to interpolate or predict client movement. All authoritative state changes are immediately reflected back to the client.

### Player simulation

The server maintains a simple player state:

- Position (x, y, z)
- Orientation (yaw, pitch)

Movement input is sent by the client as intent (forward, strafe, vertical) along with mouse deltas and frame delta time. The server applies this input directly and sends back updated state snapshots.

This model is intentionally simple and suitable for early prototyping and experimentation.

### Terminal interpreter

The server hosts a small, stateful “toy” terminal interpreter. Features include:

- Arithmetic expression evaluation
- Variable assignment and lookup
- print(expr) and print("text")
- Persistent scrollback history

The server owns the terminal state entirely. Clients only submit commands and receive output lines.

On connection, the server sends the full terminal history to the client, followed by incremental updates after each command.

---

## Client

### Responsibilities

The client is responsible for:

- Capturing keyboard and mouse input
- Sending input and terminal commands to the server
- Rendering the 3D scene
- Performing light client-side prediction and smoothing
- Rendering a terminal UI to an in-world display

The client never mutates authoritative state.

### Rendering pipeline

Rendering is split into multiple passes:

1. Terminal UI is rendered into an offscreen render texture.
2. The 3D scene is rendered into a low-resolution render texture (PS1-style).
3. The terminal texture is drawn onto a monitor surface in the 3D world.
4. The low-resolution scene is upscaled to the window using point filtering.
5. An optional post-process shader applies color quantization and dithering.

Camera position and orientation are quantized to a fixed grid to emulate low-precision hardware artifacts.

### In-world terminal

The terminal is not a HUD overlay. It exists as an object in the 3D world:

- Clicking the monitor focuses terminal input
- Keyboard input is routed to the terminal only when focused
- Terminal text is rendered to a texture and mapped onto the monitor surface
- All terminal logic runs on the server

This design allows the terminal to act as a diegetic interface rather than a debug console.

### Input and focus model

The client supports three primary interaction states:

- Free movement (mouse-look + WASD)
- Terminal-focused input
- Paused (cursor released)

ESC toggles focus and pause depending on the current state. Mouse capture is managed explicitly to avoid conflicts with typing and window resizing.

---

## Networking

### Protocol

Communication uses a simple ASCII, line-based TCP protocol.

Client to server:

HELLO  
INPUT <fwd> <right> <up> <yawDelta> <pitchDelta> <dt>  
CMD <text...>  

Server to client:

WELCOME <version>  
STATE <x> <y> <z> <yaw> <pitch>  
HIST <n>  
LINE <text...>  

Messages are newline-delimited. The protocol is designed to be human-readable and easy to debug.

### Design goals

- Simplicity over efficiency
- Explicit state transfer
- Easy inspection with raw sockets or logging
- Clear extension points for future features

---

## Build

The project is intended to be built with MinGW gcc on Windows.

Requirements:

- raylib (static library)
- OpenGL
- Winsock

Build script:

build.bat

Outputs:

bin/server.exe  
bin/client.exe  

---

## Notes and future work

This project is a prototype and intentionally incomplete. Likely extension points include:

- Multiple clients
- Tick-based simulation
- Proper interpolation and reconciliation
- World interaction via terminal commands
- Richer server-side scripting

The codebase is structured to support experimentation rather than production use.
