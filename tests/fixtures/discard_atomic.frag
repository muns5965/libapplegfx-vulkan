#version 450
// Hand-written GLSL for tests/air2spv-discard-robust-lavapipe.c (NOT translator output): atomics and stores AFTER a discard of the same invocation.
// Compiled three ways (see gen-discard-atomic-spv.sh): MODE_KILL = `discard` (glslang emits OpKill), MODE_TERM = terminateInvocation (OpTerminateInvocation), MODE_DEMOTE = demote (OpDemoteToHelperInvocation).
#extension GL_EXT_terminate_invocation : enable
#extension GL_EXT_demote_to_helper_invocation : enable
layout(set = 0, binding = 0, std430) buffer B { uint counter; uint data[64]; } b;
layout(location = 0) in vec4 pos;
layout(location = 0) out vec4 o;
void main() {
    if (pos.x < 4.0) {
#if defined(MODE_DEMOTE)
        demote;
#elif defined(MODE_TERM)
        terminateInvocation;
#else
        discard;
#endif
    }
    atomicAdd(b.counter, 1u);
    b.data[uint(pos.y) * 8u + uint(pos.x)] = 7u;
    o = vec4(1.0, 0.0, 0.0, 1.0);
}
