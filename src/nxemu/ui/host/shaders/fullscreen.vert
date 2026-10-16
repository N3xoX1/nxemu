#version 450

layout(push_constant) uniform Constants
{
    vec2 uvScale;
    vec2 uvOffset;
} constants;

layout(location = 0) out vec2 fragTexCoord;

// One triangle covering the whole target, without vertex buffers.
void main()
{
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragTexCoord = corner * constants.uvScale + constants.uvOffset;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
