# Fragment color input reads

RSX fragment reads of COL0 and COL1 clamp every component to [0,1] before
source swizzle, abs and negate. The clamp belongs at the fragment input
expression; vertex output registers and texture coordinates stay unbounded.
The shared input expression is used by all three instruction operands and
by both legacy and buffered decompilation APIs.

The production change is limited to the two COL0/COL1 input expressions.
No render-target, cache, vertex-program, tone mapping, texture or shader-control
behavior changes.

Primary oracle: RPCS3 FragmentProgramDecompiler.cpp GetSRC documents hardware
tests and explicitly saturates input registers1/2 before source modifiers.
https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp

NV_vertex_program also documents normalized color attributes at the fragment
boundary, while texture-coordinate results remain unrestricted.
https://registry.khronos.org/OpenGL/extensions/NV/NV_vertex_program.txt
