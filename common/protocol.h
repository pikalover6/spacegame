#ifndef PROTOCOL_H
#define PROTOCOL_H

// Simple line-based TCP protocol
// Client -> Server:
//   HELLO
//   INPUT <fwd> <right> <up> <yawDelta> <pitchDelta> <dt>
//   CMD <text...>            (toy terminal command)
// Server -> Client:
//   WELCOME <version>
//   STATE <x> <y> <z> <yaw> <pitch>
//   HIST <n>
//   LINE <text...>
//   PROMPT                  (signals prompt line exists already)
// Notes:
// - All messages are ASCII lines terminated by '\n'.
// - Server may send LINE messages anytime (terminal output/history).
// - Client keeps last prompt line as ">>> " and overlays typed input.

#define PROTO_VERSION "0.1"

#endif
